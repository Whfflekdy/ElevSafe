// main.cpp
// 정제 파이프라인 실행기
// 현재: 격자 번호 -> 좌표 변환 -> ROI 필터 -> 정적 배경 제거 (Step 1~2)
// 이후: -> 정적 배경 제거 (Step 2) -> SOR/RCS (Step 3) -> DBSCAN (Step 4) -> BBox (Step 5)
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

        // TODO(Step 3): outlier_filter (RCS, SOR)
        // TODO(Step 4): dbscan
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

    if (!writeFramesCSV(output_path, outputs)) return 1;
    std::cout << "출력 저장: " << output_path << "\n";
    return 0;
}