// signal_pipeline.h
// B 신호처리의 공개 진입점. 백엔드(A)는 이 헤더만 include 해서 쓴다.
//
//   SignalPipeline signal(cfg);                        // 시작할 때 한 번
//   PipelineResult r = signal.process(frame, ctx);     // 프레임마다
//
// frame: RadarFrame 을 A 의 Adapter 가 바꾼 FrameData (센서 원본 좌표 그대로)
// 처리 순서: 격자 번호 → 좌표 변환 → 문 상태 추정 → ROI → 정적 배경 제거 → DBSCAN → 덩어리 요약 → 유령 거르기
#pragma once

#include <cstdint>
#include <vector>

#include "common/config.h"
#include "common/door_state.h"
#include "common/radar_point.h"
#include "preprocessing/coordinate_transform.h"
#include "preprocessing/polar_grid.h"
#include "preprocessing/roi_filter.h"
#include "preprocessing/background_subtractor.h"
#include "preprocessing/door_state_estimator.h"
#include "clustering/dbscan.h"
#include "clustering/peak_splitter.h"    
#include "preprocessing/outlier_filter.h"   

namespace elevsafe::sp {

// A → B: 센서 데이터가 아닌 상황 정보 (필요해지면 칸만 추가)
struct FrameContext {
    DoorState door_state = DoorState::UNKNOWN;  // 백엔드가 정한 문 상태 (지금은 B 가 참고만)
};

// B → C: 덩어리 하나의 요약 (C 와 합의한 형식, 덩어리 요약 CSV 의 한 줄과 같은 내용)
struct Cluster {
    int cluster_id = -1;          // 프레임 안의 덩어리 번호 (프레임마다 새로 매김)
    // 중심: 덩어리 점들의 평균 위치 (공통 좌표, m)
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    // 범위: 덩어리를 감싸는 축 정렬 상자 (Bounding Box)
    float x_min = 0.0f, x_max = 0.0f;
    float y_min = 0.0f, y_max = 0.0f;
    float z_min = 0.0f, z_max = 0.0f;

    // z_top (float, 덩어리 점 높이의 상위 95%. 머리 높이 추정)
    float z_top = 0.0f;
    
    float mean_doppler = 0.0f; // 평균 속도(m/s, 센서 시선 방향: 다가오면 -, 멀어지면 +)
    int num_points = 0;        // 덩어리에 속한 점 수
    Zone zone = Zone::NONE;    // 덩어리 점 중 가장 많은 zone(DOOR/CABIN/HALL, 같으면 DOOR > CABIN > HALL)
    bool maybe_merged = false; // 여러 사람이 합쳐졌을 수 있음
    // --- 추가 칸 (C 요청, 11/18). 기존 칸 순서는 그대로 두고 뒤에 붙임 ---
    float doppler_std = 0.0f;  // 점 속도의 표준편차 (m/s). 정지 상태(IMMOBILE) 판단용
    float power_sum = 0.0f;    // 점 power의 합 (선형 값). 정지 상태 판단용
    float z_p10 = 0.0f;        // 점 높이 하위 10% (m). 낙상 높이 분포용
    float z_p50 = 0.0f;        // 점 높이 중앙값 (m). 낙상 높이 분포용
};

// 유령 덩어리 거르기 (11/24, A 의 시연용 40점 필터를 B 로 옮김)
//   유령 = 점이 min_points 미만, 또는
//          중심이 뒷벽 띠(cy ≥ back_y)나 센서 바로 아래(수평 sensor_radius 안)이고 점이 zone_points 미만
//   근거: 4차 S1 6장면 비교 (tools/dbscan_compare.py). 뒷벽에 기대 선 큰 덩어리(사람)는 zone_points 이상이라 남음
struct GhostFilterConfig {
    bool enabled = false;         // 기본은 꺼짐 (cfg 로 켬)
    int min_points = 20;          // 어디서든 이보다 작으면 유령
    int zone_points = 60;         // 뒷벽 띠·센서 아래에서는 이보다 작으면 유령
    float back_y = 1.30f;         // 뒷벽 띠 시작 y (뒷벽은 +1.49)
    float sensor_x = 0.0f;        // 센서 수평 위치 (cfg 의 sensor.tx, sensor.ty)
    float sensor_y = 0.0f;
    float sensor_radius = 0.40f;  // 센서 바로 아래로 볼 수평 반경

    static GhostFilterConfig fromConfig(const Config& cfg);
    bool isGhost(const Cluster& c) const;
};

// B → A: 프레임 하나의 처리 결과
struct PipelineResult {
    int frame_id = -1;
    std::int64_t timestamp_us = 0;
    std::vector<Cluster> clusters;                          // C 에게 넘길 덩어리 (없으면 빈 목록)
    DoorState estimated_door_state = DoorState::UNKNOWN;    // 레이더로 추정한 문 상태
    bool hall_enabled = false;   // HALL 구역을 출력하는지 (설정값). false면 C는 승강장 쪽을 UNKNOWN으로 취급
};

// 오프라인 실행기(main.cpp)의 결과 요약용 누적 통계. 백엔드는 안 써도 됨
struct PipelineStats {
    std::size_t frames = 0;
    ROIStats roi;
    BGStats bg;
    OutlierStats outlier;
    std::size_t clusters = 0;   // 모든 프레임의 덩어리 수 합
    std::size_t noise = 0;      // 노이즈 점 수 합
    std::size_t splits = 0;                            // PipelineStats 에 추가
    std::size_t merged = 0;
    std::size_t ghosts = 0;     // 유령 거르기로 뺀 덩어리 수 합
};

class SignalPipeline {
public:
    // 설정으로 부품을 한 번 준비. 설정이 잘못됐으면 ok() 가 false
    explicit SignalPipeline(const Config& cfg);
    bool ok() const { return ok_; }

    // 프레임 하나 처리.
    // processed_points 를 넘기면 ROI·배경 제거·DBSCAN 을 거친 점(zone, cluster_id 포함)을 채워 준다.
    // 오프라인 점 단위 CSV 용이고, 백엔드는 nullptr (기본값) 로 부르면 된다.
    PipelineResult process(const FrameData& frame, const FrameContext& ctx,
                           Frame* processed_points = nullptr);

    bool usesBackground() const { return use_bg_; }
    bool ghostFilterEnabled() const { return ghost_.enabled; }
    const PipelineStats& stats() const { return stats_; }

private:
    // 덩어리 요약 함수 선언
    // 점들(kept)과 DBSCAN 결과(labels, num_clusters)를 받아 덩어리마다 클러스터 하나를 만든다
    // 노이즈(label -1) 점은 건너뛴다. 덩어리가 없으면 빈 목록. 
    static std::vector<Cluster> summarize(const Frame& points, const SplitResult& sr);

    bool ok_ = false;
    bool use_bg_ = false;
    CoordinateTransform transform_;
    PolarGrid polar_;
    ROIFilter roi_;
    BackgroundSubtractor bg_;
    OutlierFilter outlier_;
    DBSCAN dbscan_;
    PeakSplitter splitter_;     
    GhostFilterConfig ghost_;
    DoorStateEstimator door_est_;   // 프레임 사이 상태를 기억 → process 는 const 아님
    PipelineStats stats_;
};

}  // namespace elevsafe::sp
