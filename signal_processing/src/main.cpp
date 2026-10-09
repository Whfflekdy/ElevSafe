// main.cpp
// 정제 파이프라인 실행기
// 현재: 격자 번호 -> 좌표 변환 -> ROI 필터 -> 정적 배경 제거 (Step 1~2)
// 현재: 격자 번호 -> 좌표 변환 -> ROI 필터 -> 정적 배경 제거 -> DBSCAN (Step 1, 2, 4)
// 이후: SOR/RCS (Step 3), BBox (Step 5)
//
// 사용법: ./build/radar_pipeline [config] [input.csv] [output.csv]

#include <iostream>
#include <string>

#include "common/config.h"
#include "io/csv_io.h"
#include "preprocessing/coordinate_transform.h"
#include "preprocessing/roi_filter.h"
#include "preprocessing/polar_grid.h"
#include "preprocessing/background_subtractor.h"
#include "clustering/dbscan.h"   // 맨 위 include 에 추가

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
        out.timestamp_ms = fd.timestamp_ms;
        out.points = std::move(kept);
        outputs.push_back(std::move(out));
    }

    // 5. 결과 요약 출력 및 저장
    std::cout << "=== 결과 (" << frames.size() << " frames) ===\n"
              << "입력 포인트 : " << total.input   << "\n"
              << "DOOR        : " << total.door    << "\n"
              << "CABIN       : " << total.cabin   << "\n"
              << "ROI 제거    : " << total.dropped << "\n";
    if (use_bg) std::cout << "배경 제거   : " << bg_total.removed << "\n";
    std::cout << "[" << clusterer.name() << "] 프레임당 평균 덩어리 "
          << static_cast<double>(total_clusters) / frames.size()
          << "개, 노이즈 점 " << total_noise << "개\n";
    if (!writeFramesCSV(output_path, outputs)) return 1;
    std::cout << "출력 저장: " << output_path << "\n";
    return 0;
}