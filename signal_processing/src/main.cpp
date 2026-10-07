// main.cpp
// 정제 파이프라인 실행기
// 현재: 좌표 변환 -> ROI 필터 (Step 1)
// 이후: -> 정적 배경 제거 (Step 2) -> SOR/RCS (Step 3) -> DBSCAN (Step 4) -> BBox (Step 5)
//
// 사용법: ./build/radar_pipeline [config] [input.csv] [output.csv]

#include <iostream>
#include <string>

#include "common/config.h"
#include "io/csv_io.h"
#include "preprocessing/coordinate_transform.h"
#include "preprocessing/roi_filter.h"

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

    for (auto& fd : frames) {
        // TODO(Step 2): 좌표 변환 전에 센서 기준 극좌표 격자 번호 계산 (polar.assignBins)
        //               변환 후에는 센서 기준 좌표가 사라지므로 반드시 맨 앞에 둘 것

        // TODO: (1-a) transform 으로 frame 좌표 변환
        // TODO: (1-b) roi.filter 로 필터링 + 통계 받기, 결과를 outputs 에 추가
        // TODO: 이번 프레임 통계를 total 에 누적
        transform.apply(fd.points);

        ROIStats stats;
        FrameData out;
        out.frame_id = fd.frame_id;
        out.timestamp_ms = fd.timestamp_ms;
        out.points = roi.filter(fd.points, &stats);
        outputs.push_back(std::move(out));
        total += stats;

        // TODO(Step 2): background_subtractor
        // TODO(Step 3): outlier_filter (RCS, SOR)
        // TODO(Step 4): dbscan
        // TODO(Step 5): bbox 후처리
    }

    // 5. 결과 요약 출력 및 저장
    std::cout << "=== ROI 필터 결과 (" << frames.size() << " frames) ===\n"
              << "입력 포인트 : " << total.input   << "\n"
              << "DOOR        : " << total.door    << "\n"
              << "CABIN       : " << total.cabin   << "\n"
              << "제거됨      : " << total.dropped << "\n";

    if (!writeFramesCSV(output_path, outputs)) return 1;
    std::cout << "출력 저장: " << output_path << "\n";
    return 0;
}