// main.cpp
// 정제 파이프라인 실행기
// 현재: 격자 번호 -> 좌표 변환 -> ROI 필터 -> 정적 배경 제거 (Step 1~2)
// 현재: 격자 번호 -> 좌표 변환 -> ROI 필터 -> 정적 배경 제거 -> DBSCAN (Step 1, 2, 4)
// 이후: SOR/RCS (Step 3), BBox (Step 5)
//
// 사용법: ./build/radar_pipeline [config] [input.csv] [output.csv]

#include <iostream>
#include <string>
#include <iomanip>
#include <sstream>
#include <fstream>

#include "common/config.h"
#include "io/csv_io.h"
#include "preprocessing/coordinate_transform.h"
#include "preprocessing/roi_filter.h"
#include "preprocessing/polar_grid.h"
#include "preprocessing/background_subtractor.h"
#include "clustering/dbscan.h"
#include "preprocessing/door_state_estimator.h"

int main(int argc, char** argv) {
    // 실행할 때 경로를 넘기면 그걸 쓰고, 안 넘기면 기본 경로 사용
    const std::string config_path = argc > 1 ? argv[1] : "config/elevator_default.cfg";
    const std::string input_path  = argc > 2 ? argv[2] : "data/sample/tiny.csv";
    const std::string output_path = argc > 3 ? argv[3] : "data/output/roi_out.csv";

    // 1. 설정 읽기
    Config cfg;
    if (!cfg.load(config_path)) return 1;

    // 2. 부품 준비 (한 번만)
    const CoordinateTransform transform(SensorPose::fromConfig(cfg)); // 설정파일 -> 센서 자세 -> 변환기
    const ROIConfig roi_cfg = ROIConfig::fromConfig(cfg);             // 설정 파일 -> ROI 영역
    if(!roi_cfg.isValid()) return 1;                                  // 범위가 잘못됐으면 종료
    const DoorStateConfig door_cfg = DoorStateConfig::fromConfig(cfg);
    if (!door_cfg.isValid()) return 1;
    DoorStateEstimator door_est(door_cfg);   // 상태를 기억하므로 const 아님

    // 프레임별 문 상태 기록 (출력 파일 이름 뒤에 _door 를 붙여 따로 저장)
    // output_path 의 ".csv" 를 "_door.csv" 로 바꾼 경로 만들기
    std::string door_path = output_path;
    const std::string ext = ".csv";
    if (door_path.size() >= ext.size() &&
        door_path.compare(door_path.size() - ext.size(), ext.size(), ext) == 0) {
        door_path.erase(door_path.size() - ext.size());   // 끝의 ".csv" 떼기
    }
    door_path += "_door.csv";

    // 그 경로로 std::ofstream 열고 머리글 "frame_id,timestamp_us,count,average,state" 쓰기
    std::ofstream door_file(door_path);
    if (!door_file.is_open()) {
        std::cerr << "[문 상태] 파일을 쓸 수 없음: " << door_path << "\n";
        return 1;
    }

    door_file << "frame_id,timestamp_us,count,average,state\n";
    door_file << std::fixed << std::setprecision(2);   // average 를 소수 둘째 자리까지 (정수 칸에는 영향 없음)

    std::size_t open_frames = 0, transitions = 0;
    DoorState prev_state = DoorState::UNKNOWN;
    
    const PolarGrid polar(PolarGridConfig::fromConfig(cfg));
    BackgroundSubtractor bg(BackgroundConfig::fromConfig(cfg));
    const std::string map_path = cfg.getString("background.map_path", "");
    const bool use_bg = !map_path.empty() && bg.load(map_path);
    if (use_bg) std::cout << "[배경 제거] 배경 칸 " << bg.backgroundCellCount() << "개 사용\n";
    else        std::cout << "[배경 제거] 배경 맵 없음 → 건너뜀\n";
    const DBSCAN dbscan(DBSCANConfig::fromConfig(cfg));
    const Clusterer& clusterer = dbscan; // 파이프라인은 공통 틀(Clusterer)로만 사용. (추후 새로운 알고리즘 도입 및 DBSCAN과 비교 예정)
        // 나중에 이렇게만 바꾸면 됨
    //const MyClusterer mine(MyClustererConfig::fromConfig(cfg));
    //const Clusterer& clusterer = mine;

    const ROIFilter roi(roi_cfg);
    // 3. 데이터 읽기
    std::vector<FrameData> frames = readFramesCSV(input_path);
    if (frames.empty()) {
        std::cerr << "입력 프레임이 없습니다: " << input_path << "\n";
        return 1;
    }

    // 4. 프레임마다 파이프라인 실행
    std::vector<FrameData> outputs;
    outputs.reserve(frames.size());
    ROIStats total;
    BGStats bg_total;

    std::size_t total_clusters = 0;   // 모든 프레임의 덩어리 수 합
    std::size_t total_noise = 0;      // 노이즈로 분류된 점 수

    for (auto& fd : frames) {
        polar.assignBins(fd.points); // 격자 번호(센서 좌표)
        transform.apply(fd.points);  // 좌표 변환.

        const DoorState ds = door_est.update(fd.points);  // ROI 전에!
        // door 파일에 frame_id, timestamp_us, lastCount(), average(), 상태 문자열 한 줄 쓰기
        door_file << fd.frame_id << ',' << fd.timestamp_us << ','
                  << door_est.lastCount() << ',' << door_est.average() << ','
                  << doorStateToString(ds) << '\n';
        // ds 가 OPEN 이면 open_frames++
        if (ds == DoorState::OPEN) ++open_frames;
        // prev_state 가 UNKNOWN 이 아니고 ds 와 다르면 transitions++, prev_state = ds
        //       (prev_state 가 UNKNOWN 이면 그냥 prev_state = ds)
        if (prev_state != DoorState::UNKNOWN && ds != prev_state) ++transitions;
        prev_state = ds;   // 처음 UNKNOWN → 첫 판단은 전환으로 세지 않음

        ROIStats stats;
        Frame kept = roi.filter(fd.points, &stats); // ROI
        total+= stats;

        if(use_bg){                 // 정적 배경 제거
            BGStats bs;
            kept = bg.filter(kept, &bs);
            bg_total.input += bs.input;
            bg_total.removed += bs.removed;
        }

        // Step 3: outlier_filter (RCS, SOR)

        // Step 4: dbscan = 점마다 덩어리 번호 기록
        const ClusteringResult cr = clusterer.cluster(kept);
        for (std::size_t i = 0; i < kept.size(); ++i) {
            kept[i].cluster_id = cr.labels[i];
            if(cr.labels[i]==-1)  // 노이즈인 개수를 total에 더하기
                total_noise++;
        }
        total_clusters += cr.num_clusters;
        
        // TODO(Step 5): bbox 후처리

        FrameData out;
        out.frame_id = fd.frame_id;
        out.timestamp_us = fd.timestamp_us;
        out.points = std::move(kept);
        outputs.push_back(std::move(out));
    }

    // 5. 결과 요약 출력 및 저장
    std::cout << "=== 결과 (" << frames.size() << " frames) ===\n"
              << "입력 포인트 : " << total.input   << "\n"
              << "DOOR        : " << total.door    << "\n"
              << "CABIN       : " << total.cabin   << "\n"
              << "ROI 제거    : " << total.dropped << "\n";
    if (use_bg) {
        std::cout << "배경 제거   : " << bg_total.removed;
        if (bg_total.input > 0) {
            const double ratio = static_cast<double>(bg_total.removed) / bg_total.input * 100.0;
            std::ostringstream pct;
            pct << std::fixed << std::setprecision(1) << ratio;
            std::cout << " (ROI 통과 점의 " << pct.str() << "%)";
        } else {
            std::cout << " (ROI 통과 점 없음 → 설치 실측값 확인)";
        }
        std::cout << "\n";
    }
    std::cout << "[" << clusterer.name() << "] 프레임당 평균 덩어리 "
      << static_cast<double>(total_clusters) / frames.size()
      << "개, 노이즈 점 " << total_noise << "개\n";
    if (!writeFramesCSV(output_path, outputs)) return 1;
    std::cout << "출력 저장: " << output_path << "\n";
    std::cout << "[문 상태] OPEN 프레임 " << open_frames << " / " << frames.size()
      << ", 상태 전환 " << transitions << "번 → " << door_path << "\n";
    return 0;
}