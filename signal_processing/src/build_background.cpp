// build_background.cpp
// 빈 장면 CSV 로 배경 맵을 만들어 파일로 저장한다.
// 격자 번호는 센서 좌표 기준이라 좌표 변환 없이 바로 계산한다.
//
// 사용법: ./build/build_background [config] [empty.csv] [map.csv]

#include <iostream>
#include <string>

#include "common/config.h"
#include "io/csv_io.h"
#include "preprocessing/background_subtractor.h"
#include "preprocessing/polar_grid.h"

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/elevator_default.cfg";
    const std::string empty_path  = argc > 2 ? argv[2] : "data/raw/empty.csv";
    const std::string map_path    = argc > 3 ? argv[3] : "data/background/map.csv";

    Config cfg;
    if (!cfg.load(config_path)) return 1;

    const PolarGrid polar(PolarGridConfig::fromConfig(cfg));
    BackgroundSubtractor bg(BackgroundConfig::fromConfig(cfg));

    std::vector<FrameData> frames = readFramesCSV(empty_path);
    if (frames.empty()) {
        std::cerr << "빈 장면 프레임이 없습니다: " << empty_path << "\n";
        return 1;
    }

    // 모든 프레임에 polar.assignBins 적용
    for(auto& fd: frames) polar.assignBins(fd.points);
    // bg.build(frames) 로 배경 맵 생성
    bg.build(frames);
    // bg.save(map_path) 가 실패하면 return 1
    if(!bg.save(map_path)) return 1;

    std::cout << "빈 장면 " << frames.size() << " 프레임 → 배경 칸 "
              << bg.backgroundCellCount() << "개\n"
              << "저장: " << map_path << "\n";
    return 0;
}