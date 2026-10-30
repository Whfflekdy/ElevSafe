// main.cpp
// 오프라인 실행기: CSV 를 SignalPipeline 에 한 프레임씩 넣고 결과를 파일로 남긴다.
// 백엔드도 같은 SignalPipeline 을 쓰므로, 여기서 확인한 결과가 곧 실시간 결과다.
//
// 사용법: ./build/radar_pipeline [config] [input.csv] [output.csv]
// 출력: output.csv (점 단위), output_door.csv (문 상태), output_clusters.csv (덩어리 요약, B→C 형식)

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "common/config.h"
#include "io/csv_io.h"
#include "pipeline/signal_pipeline.h"

using namespace elevsafe::sp;

// output.csv → output<suffix>.csv  (예: "_door")
static std::string withSuffix(const std::string& path, const std::string& suffix) {
    std::string base = path;
    const std::string ext = ".csv";
    if (base.size() >= ext.size() &&
        base.compare(base.size() - ext.size(), ext.size(), ext) == 0) {
        base.erase(base.size() - ext.size());   // 끝의 ".csv" 떼기
    }
    return base + suffix + ext;
}

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config/elevator_default.cfg";
    const std::string input_path  = argc > 2 ? argv[2] : "data/sample/tiny.csv";
    const std::string output_path = argc > 3 ? argv[3] : "data/output/roi_out.csv";

    // 1. 설정과 파이프라인 (한 번)
    Config cfg;
    if (!cfg.load(config_path)) return 1;
    SignalPipeline pipeline(cfg);
    if (!pipeline.ok()) { std::cerr << "설정이 올바르지 않습니다: " << config_path << "\n"; return 1; }
    if (pipeline.usesBackground()) std::cout << "[배경 제거] 배경 맵 사용\n";
    else                           std::cout << "[배경 제거] 배경 맵 없음 → 건너뜀\n";

    // 2. 데이터
    std::vector<FrameData> frames = readFramesCSV(input_path);
    if (frames.empty()) { std::cerr << "입력 프레임이 없습니다: " << input_path << "\n"; return 1; }

    // 3. 출력 파일 두 개 (문 상태, 덩어리 요약)
    const std::string door_path = withSuffix(output_path, "_door");
    const std::string cl_path   = withSuffix(output_path, "_clusters");

    // door_file 열고 머리글 "frame_id,timestamp_us,state" (count·average 는 이제 파이프라인 안이라 빠짐)
    std::ofstream door_file(door_path);
    std::ofstream cl_file(cl_path);
    // 둘 다 열기 실패하면 에러 메시지 후 return 1
    if (!door_file.is_open() || !cl_file.is_open()) {
        std::cerr << "출력 파일을 쓸 수 없음: " << door_path << " / " << cl_path << "\n";
        return 1;
    }
    // 두 파일 모두 std::fixed << std::setprecision(3)
    door_file << "frame_id,timestamp_us,state\n";
    cl_file << "frame_id,timestamp_us,cluster_id,cx,cy,cz,x_min,x_max,y_min,y_max,"
               "z_min,z_max,z_top,mean_doppler,num_points,zone,maybe_merged\n";
    door_file << std::fixed << std::setprecision(3);
    cl_file   << std::fixed << std::setprecision(3);   // m 기준 mm 까지. 정수 칸에는 영향 없음

    // 4. 프레임마다
    std::vector<FrameData> outputs;
    outputs.reserve(frames.size());
    std::size_t open_frames = 0, transitions = 0;
    DoorState prev_state = DoorState::UNKNOWN;
    const FrameContext ctx;   // 오프라인에는 백엔드 문 상태가 없으니 기본값(UNKNOWN)

    for (const auto& fd : frames) {
        Frame pts;
        const PipelineResult r = pipeline.process(fd, ctx, &pts);

        // 점 단위 출력 (기존과 같은 파일)
        FrameData out;
        out.frame_id = r.frame_id;
        out.timestamp_us = r.timestamp_us;
        out.points = std::move(pts);
        outputs.push_back(std::move(out));

        // TODO: 문 상태 한 줄 쓰기, open_frames·transitions 세기 (기존 코드와 같은 규칙)
        const DoorState ds = r.estimated_door_state;
        door_file << r.frame_id << ',' << r.timestamp_us << ',' << doorStateToString(ds) << '\n';
        if (ds == DoorState::OPEN) ++open_frames;
        if (prev_state != DoorState::UNKNOWN && ds != prev_state) ++transitions;
        prev_state = ds;

        // 덩어리 요약 (C 와 합의: 덩어리가 0개인 프레임도 frame_id, timestamp_us 만 있는 줄로 남김)
        if (r.clusters.empty()) {
            cl_file << r.frame_id << ',' << r.timestamp_us << '\n';
        }
        //   아니면 덩어리마다 한 줄. zone 은 zoneToString, maybe_merged 는 0/1
        for (const Cluster& c : r.clusters) {
            cl_file << r.frame_id << ',' << r.timestamp_us << ',' << c.cluster_id << ','
                    << c.cx << ',' << c.cy << ',' << c.cz << ','
                    << c.x_min << ',' << c.x_max << ','
                    << c.y_min << ',' << c.y_max << ','
                    << c.z_min << ',' << c.z_max << ','
                    << c.z_top << ',' << c.mean_doppler << ',' << c.num_points << ','
                    << zoneToString(c.zone) << ',' << (c.maybe_merged ? 1 : 0) << '\n';
        }

    }

    // 5. 요약 (기존과 같은 형식, 숫자는 pipeline.stats() 에서)
    const PipelineStats& s = pipeline.stats();
    // 기존 main.cpp 의 "=== 결과 ===" 블록을 옮기되,
    // total → s.roi, bg_total → s.bg, total_clusters → s.clusters, total_noise → s.noise
    // 배경 제거 줄은 pipeline.usesBackground() 일 때만
    // DBSCAN 줄의 clusterer.name() 은 "DBSCAN" 으로 직접
    std::cout << "=== 결과 (" << frames.size() << " frames) ===\n"
              << "입력 포인트 : " << s.roi.input   << "\n"
              << "DOOR        : " << s.roi.door    << "\n"
              << "CABIN       : " << s.roi.cabin   << "\n"
              << "ROI 제거    : " << s.roi.dropped << "\n";
    if (pipeline.usesBackground()) {
        std::cout << "배경 제거   : " << s.bg.removed;
        if (s.bg.input > 0) {
            const double ratio = static_cast<double>(s.bg.removed) / s.bg.input * 100.0;
            std::ostringstream pct;
            pct << std::fixed << std::setprecision(1) << ratio;
            std::cout << " (ROI 통과 점의 " << pct.str() << "%)";
        } else {
            std::cout << " (ROI 통과 점 없음 → 설치 실측값 확인)";
        }
        std::cout << "\n";
    }
    std::cout << "[DBSCAN] 프레임당 평균 덩어리 "
              << static_cast<double>(s.clusters) / frames.size()
              << "개, 노이즈 점 " << s.noise << "개\n";

    if (!writeFramesCSV(output_path, outputs)) return 1;
    std::cout << "출력 저장: " << output_path << "\n";
    std::cout << "[문 상태] OPEN 프레임 " << open_frames << " / " << frames.size()
              << ", 상태 전환 " << transitions << "번 → " << door_path << "\n";
    std::cout << "[덩어리 요약] → " << cl_path << "\n";
    return 0;
}