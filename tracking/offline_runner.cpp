// tracking/offline_runner.cpp
// 클러스터 CSV(*_clusters.csv) -> 프레임별 트랙 결과 CSV + 요약 파일(자동 저장)
// Usage: tracking_offline <input_clusters.csv> [result.csv] [gate_xy=0.8 n_init=3 max_age_s=5 ...]
//   상태 판단 옵션: door=OPEN|CLOSED|UNKNOWN (기본 UNKNOWN -> door_command 는 항상 null)
//                   hall=0|1 (기본 0: HALL 덩어리 버림), immobile_sec, fall_drop_m, fall_speed_mps,
//                   recover_hold_sec, hold_max_sec, close_wait_sec, lost_grace_sec, lost_grace_danger_sec
//   2차 녹화는 대부분 문 열림이라 door=OPEN 을 줘야 HOLD/CLOSE 를 볼 수 있다 (README 3장 "문 상태 정답").
//   문 상태 시나리오 (우선순위: door= < door_csv= < door_close_*):
//     door=OPEN|CLOSED|UNKNOWN   파일 전체에 이 값 하나 적용 (주면 door.csv 자동 읽기는 꺼짐)
//     (door=, door_csv= 둘 다 없으면 <입력>_clusters.csv -> <입력>_door.csv 가 있을 때 자동으로 읽는다)
//     door_auto=0                자동 읽기를 끔 (door 없이 돌리고 싶을 때)
//     door_csv=<path>            프레임별 문 상태를 *_door.csv 에서 읽음 (frame_id 로 매칭, 없는 프레임은 UNKNOWN)
//     door_close_frame=N         frame_id 가 N 이상이면 CLOSED ("열림 -> 마지막에 닫힘" 장면용)
//     door_close_sec=S           첫 프레임부터 S초 이상 지나면 CLOSED
//   예) tracking_offline S2-4_t01_clusters.csv door=OPEN door_close_sec=9.5
//   result.csv를 생략하면 입력 파일 이름으로 자동 생성: <입력이름>_result.csv
//   요약은 콘솔 출력과 함께 <result 이름>_summary.txt 로도 저장된다.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include "id_manager/id_manager.hpp"
#include "detection/cluster_csv_loader.hpp"
#include "detection/cluster_loader.hpp"   // loadDoorCsv / doorAt (door_csv 옵션용)
#include "tracking_api.hpp"

static const char* stateName(TrackState s) {
    switch (s) {
        case TrackState::TENTATIVE: return "TENTATIVE";
        case TrackState::CONFIRMED: return "CONFIRMED";
        case TrackState::LOST:      return "LOST";
    }
    return "?";
}

// Apply a "key=value" argument to TrackerConfig.
// 프레임마다 문 상태를 정하는 계획
struct DoorPlan {
    elevsafe::DoorState base = elevsafe::DoorState::UNKNOWN;
    bool base_set = false;                                 // door= 를 직접 줬는가
    bool auto_csv = true;                                  // 같은 이름 door.csv 자동 탐색
    std::string csv_path;                                  // 비어 있으면 door.csv 미사용
    std::map<int64_t, elevsafe::DoorState> csv;
    int64_t close_frame = -1;                              // -1 = 미사용
    double close_sec = -1;                                 // -1 = 미사용
    int64_t t0_us = 0;                                     // 첫 프레임 시각

    elevsafe::DoorState at(int64_t frame, int64_t ts_us) const {
        elevsafe::DoorState s = base;
        if (!csv_path.empty()) s = elevsafe::doorAt(csv, frame);
        if (close_frame >= 0 && frame >= close_frame) s = elevsafe::DoorState::CLOSED;
        if (close_sec >= 0 && (ts_us - t0_us) * 1e-6 >= close_sec) s = elevsafe::DoorState::CLOSED;
        return s;
    }
    std::string describe() const {
        std::string d = csv_path.empty() ? std::string(elevsafe::toString(base)) : "csv(" + csv_path + ")";
        if (close_frame >= 0) d += " ->CLOSED@frame" + std::to_string(close_frame);
        if (close_sec >= 0) d += " ->CLOSED@" + std::to_string(close_sec) + "s";
        return d;
    }
};

static bool applyOption(TrackerConfig& c, ClusterParams& cp, int& min_points,
                        elevsafe::Config& sc, DoorPlan& door, const std::string& kv) {
    auto p = kv.find('=');
    if (p == std::string::npos) return false;
    std::string k = kv.substr(0, p);
    if (k == "door") {   // 문자열 값
        const std::string val = kv.substr(p + 1);
        if (val != "OPEN" && val != "CLOSED" && val != "UNKNOWN") return false;
        door.base = elevsafe::parseDoorState(val);
        door.base_set = true;
        return true;
    }
    if (k == "door_csv") {   // 경로 문자열
        door.csv_path = kv.substr(p + 1);
        return !door.csv_path.empty();
    }
    double v = std::stod(kv.substr(p + 1));
    if      (k == "gate_xy")              c.gate_xy = v;
    else if (k == "gate_z")               c.gate_z = v;
    else if (k == "z_weight")             c.z_weight = v;
    else if (k == "n_init")               c.n_init = (int)v;
    else if (k == "max_age_s")            c.max_age_s = v;
    else if (k == "tentative_max_gap_s")  c.tentative_max_gap_s = v;
    else if (k == "lost_vel_decay_per_s") c.lost_vel_decay_per_s = v;
    else if (k == "q_xy")                 c.q_xy = v;
    else if (k == "q_z")                  c.q_z = v;
    else if (k == "r_xy")                 c.r_xy = v;
    else if (k == "r_z")                  c.r_z = v;
    else if (k == "eps_xy")               cp.eps_xy = v;
    else if (k == "z_scale")              cp.z_scale = v;
    else if (k == "cluster_min_pts")      cp.min_pts = (int)v;
    else if (k == "min_points")           min_points = (int)v;
    else if (k == "hall")                 sc.hall_enabled = (v != 0);
    else if (k == "door_auto")            door.auto_csv = (v != 0);
    else if (k == "door_close_frame")     door.close_frame = (int64_t)v;
    else if (k == "door_close_sec")       door.close_sec = v;
    else if (k == "immobile_sec")         sc.immobile_sec = v;
    else if (k == "fall_drop_m")          sc.fall_drop_m = v;
    else if (k == "fall_speed_mps")       sc.fall_speed_mps = v;
    else if (k == "recover_hold_sec")     sc.recover_hold_sec = v;
    else if (k == "hold_max_sec")         sc.hold_max_sec = v;
    else if (k == "close_wait_sec")       sc.close_wait_sec = v;
    else if (k == "lost_grace_sec")       sc.lost_grace_sec = v;
    else if (k == "lost_grace_danger_sec") sc.lost_grace_danger_sec = v;
    else return false;
    return true;
}

// "dir/S0-1_t01_out_clusters.csv" -> "S0-1_t01_out_clusters"
static std::string fileStem(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    std::string name = (s == std::string::npos) ? path : path.substr(s + 1);
    size_t d = name.find_last_of('.');
    return (d == std::string::npos) ? name : name.substr(0, d);
}

// "result.csv" -> "result_summary.txt"
static std::string summaryPathFor(const std::string& result_path) {
    size_t d = result_path.find_last_of('.');
    size_t s = result_path.find_last_of("/\\");
    std::string base = (d == std::string::npos || (s != std::string::npos && d < s))
                           ? result_path : result_path.substr(0, d);
    return base + "_summary.txt";
}

// "result.csv" -> "result_state.csv"
static std::string statePathFor(const std::string& result_path) {
    size_t d = result_path.find_last_of('.');
    size_t s = result_path.find_last_of("/\\");
    std::string base = (d == std::string::npos || (s != std::string::npos && d < s))
                           ? result_path : result_path.substr(0, d);
    return base + "_state.csv";
}

struct IdInfo {
    int64_t first_frame = 0, last_frame = 0;
    int64_t first_ts = 0, last_ts = 0;
    int n_frames = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <input_clusters.csv> [result.csv] [gate_xy=0.8 n_init=3 max_age_s=5 ...]\n";
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_path = fileStem(in_path) + "_result.csv";   // 입력마다 다른 이름 -> 덮어쓰기 방지
    TrackerConfig cfg;
    ClusterParams cparams;   // 클러스터 CSV에서는 쓰지 않음 (옵션 호환용)
    int min_points = 3;
    elevsafe::Config scfg;                                  // 상태 판단 설정 (룰표 v2)
    DoorPlan plan;                                          // 문 상태 시나리오
    elevsafe::DoorState door = elevsafe::DoorState::UNKNOWN;  // 현재 프레임의 문 상태

    int i = 2;
    if (argc > 2 && std::string(argv[2]).find('=') == std::string::npos) {
        out_path = argv[2];
        i = 3;
    }
    std::string args_used;   // 요약에 남길 옵션 기록
    for (int k = i; k < argc; ++k) args_used += std::string(k > i ? " " : "") + argv[k];

    try {
        for (; i < argc; ++i) {
            if (!applyOption(cfg, cparams, min_points, scfg, plan, argv[i])) {
                std::cerr << "Unknown option: " << argv[i] << "\n";
                return 1;
            }
        }

        std::cout << "Loading: " << in_path << std::endl;
        auto frames = loadDetectionsFromClustersCsv(in_path, min_points);
        if (frames.empty()) {
            std::cerr << "No frames in: " << in_path << "\n";
            return 1;
        }

        plan.t0_us = frames.front().timestamp_us;
        if (plan.csv_path.empty() && !plan.base_set && plan.auto_csv) {   // 같은 이름의 door.csv 자동 탐색
            const std::string suffix = "_clusters.csv";
            if (in_path.size() > suffix.size() &&
                in_path.compare(in_path.size() - suffix.size(), suffix.size(), suffix) == 0) {
                const std::string cand = in_path.substr(0, in_path.size() - suffix.size()) + "_door.csv";
                std::ifstream probe(cand);
                if (probe) { plan.csv_path = cand; std::cout << "Door csv auto-detected: " << cand << "\n"; }
            }
        }
        if (!plan.csv_path.empty()) {
            plan.csv = elevsafe::loadDoorCsv(plan.csv_path);
            std::cout << "Door csv: " << plan.csv_path << " (" << plan.csv.size() << " rows)\n";
        }
        std::cout << "Door plan: " << plan.describe() << std::endl;

        TrackingEngine engine(cfg, cparams, min_points, scfg);   // 실시간(A)과 같은 processFrame 경로
        const TrackManager& tm = engine.manager();
        std::ofstream fo(out_path);
        if (!fo) { std::cerr << "Cannot write result file: " << out_path << "\n"; return 1; }
        fo << std::fixed << std::setprecision(3);
        fo << "frame,timestamp_us,internal_id,id,state,x,y,z,vx,vy,vz,n_points,time_since_update_s\n";

        const std::string state_path = statePathFor(out_path);
        std::ofstream fst(state_path);
        if (!fst) { std::cerr << "Cannot write state file: " << state_path << "\n"; return 1; }
        fst << std::fixed << std::setprecision(3);
        fst << "frame,timestamp_us,door_state,door_command,safety_state,hold_exceeded,door_open_limit_warning,reason,tracks(id:phase:safety:zone)\n";
        size_t cmd_hold = 0, cmd_close = 0, cmd_open = 0;
        int64_t first_fall = -1, first_immobile = -1, hold_exceeded_frames = 0;

        std::map<int, IdInfo> ids;
        size_t max_conf = 0;
        size_t removed_total = 0;
        size_t frames_with_det = 0;

        for (const auto& fd : frames) {
            if (!fd.detections.empty()) ++frames_with_det;
            door = plan.at(fd.frame, fd.timestamp_us);
            FrameResult fr = engine.processFrame(fd, door);
            const auto& tracks = tm.tracks();
            removed_total += tm.removedTracks().size();

            fst << fd.frame << ',' << fd.timestamp_us << ',' << elevsafe::toString(door) << ','
                << elevsafe::toString(fr.door_command) << ',' << elevsafe::toString(fr.safety_state) << ','
                << (fr.hold_exceeded ? 1 : 0) << ',' << (fr.door_open_limit_warning ? 1 : 0) << ','
                << fr.reason << ',';
            for (size_t k = 0; k < fr.tracks.size(); ++k) {
                const TrackOutput& o = fr.tracks[k];
                fst << (k ? "|" : "") << o.id << ':' << elevsafe::toString(o.phase) << ':'
                    << elevsafe::toString(o.safety) << ':' << elevsafe::toString(o.zone);
            }
            fst << '\n';
            if (fr.door_command == elevsafe::DoorCommand::HOLD) ++cmd_hold;
            if (fr.door_command == elevsafe::DoorCommand::CLOSE) ++cmd_close;
            if (fr.door_command == elevsafe::DoorCommand::OPEN) ++cmd_open;
            if (fr.hold_exceeded) ++hold_exceeded_frames;
            if (first_fall < 0 && fr.safety_state == elevsafe::Safety::FALL) first_fall = fd.frame;
            if (first_immobile < 0 && fr.safety_state == elevsafe::Safety::IMMOBILE) first_immobile = fd.frame;

            size_t conf = 0;
            for (const Track& t : tracks) {
                fo << fd.frame << ',' << fd.timestamp_us << ',' << t.internal_id << ',' << t.id << ','
                   << stateName(t.state) << ',' << t.x() << ',' << t.y() << ',' << t.z() << ','
                   << t.vx() << ',' << t.vy() << ',' << t.vz() << ',' << t.n_points << ','
                   << t.time_since_update_s << '\n';

                if (t.state == TrackState::CONFIRMED && t.id >= 0) {
                    ++conf;
                    auto it = ids.find(t.id);
                    if (it == ids.end()) {
                        IdInfo info;
                        info.first_frame = fd.frame; info.first_ts = fd.timestamp_us;
                        it = ids.emplace(t.id, info).first;
                    }
                    it->second.last_frame = fd.frame;
                    it->second.last_ts = fd.timestamp_us;
                    it->second.n_frames++;
                }
            }
            max_conf = std::max(max_conf, conf);
        }
        fo.close();
        fst.close();

        // 요약을 문자열로 만들어 콘솔과 파일에 동시에 기록
        double dur_s = (frames.back().timestamp_us - frames.front().timestamp_us) / 1e6;
        std::ostringstream sm;
        sm << std::fixed << std::setprecision(3);
        sm << "=== Summary ===\n"
           << "Input: " << in_path << "\n"
           << "Result: " << out_path << "\n"
           << "Options: " << (args_used.empty() ? "(default)" : args_used) << "\n"
           << "Config: gate_xy=" << cfg.gate_xy << " gate_z=" << cfg.gate_z
           << " z_weight=" << cfg.z_weight << " n_init=" << cfg.n_init
           << " max_age_s=" << cfg.max_age_s << " tentative_max_gap_s=" << cfg.tentative_max_gap_s
           << " lost_vel_decay_per_s=" << cfg.lost_vel_decay_per_s << "\n"
           << "        q_xy=" << cfg.q_xy << " q_z=" << cfg.q_z
           << " r_xy=" << cfg.r_xy << " r_z=" << cfg.r_z << " min_points=" << min_points << "\n"
           << "Frames: " << frames.size() << " (with detections: " << frames_with_det
           << ", empty: " << frames.size() - frames_with_det << ", about " << dur_s << " s)\n"
           << "Public IDs assigned: " << tm.confirmedIdCount() << "\n"
           << "Max simultaneous CONFIRMED: " << max_conf << "\n"
           << "State: door=" << plan.describe() << " hall=" << (scfg.hall_enabled ? 1 : 0)
           << " immobile_sec=" << scfg.immobile_sec << " hold_max_sec=" << scfg.hold_max_sec << "\n"
           << "Door command frames: HOLD=" << cmd_hold << " CLOSE=" << cmd_close << " OPEN=" << cmd_open
           << " (hold_exceeded frames=" << hold_exceeded_frames << ")\n"
           << "First FALL frame: " << first_fall << "  First IMMOBILE frame: " << first_immobile << "  (-1 = none)\n"
           << "Tracks removed: " << removed_total << "\n\n"
           << "Per-ID lifetime (CONFIRMED only)\n"
           << "  ID  first_frame  last_frame  duration_s  n_frames\n";
        for (const auto& [id, info] : ids) {
            sm << "  " << id << "   " << info.first_frame << "   " << info.last_frame << "   "
               << (info.last_ts - info.first_ts) / 1e6 << "   " << info.n_frames << "\n";
        }
        if (ids.empty()) sm << "  (none)\n";

        std::string sum_path = summaryPathFor(out_path);
        std::ofstream fs(sum_path);
        if (fs) fs << sm.str();
        std::cout << sm.str() << "Saved: " << out_path << "\nSaved: " << state_path << "\nSaved: " << sum_path
                  << (fs ? "" : "  (FAILED)") << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
