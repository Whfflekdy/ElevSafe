// tracking/offline_runner.cpp
// B 클러스터 CSV -> TrackingEngine::processFrame() (추적 + 상태 판단 + 문 명령) -> 결과 CSV + 요약
// Usage: tracking_offline <input_clusters.csv> [result.csv] [key=value ...]
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "tracking_api.hpp"
#include "detection/cluster_csv_loader.hpp"

static const char* stateName(TrackState s) {
    switch (s) {
        case TrackState::TENTATIVE: return "TENTATIVE";
        case TrackState::CONFIRMED: return "CONFIRMED";
        case TrackState::LOST:      return "LOST";
    }
    return "?";
}

// ---- door input -----------------------------------------------------------
struct DoorRow { std::string state = "UNKNOWN"; int hall = 0; };

static std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out; std::string cur; std::stringstream ss(line);
    while (std::getline(ss, cur, ',')) {
        while (!cur.empty() && (cur.back() == '\r' || cur.back() == ' ')) cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

static std::map<int64_t, DoorRow> loadDoorFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open door csv: " + path);
    std::string line;
    if (!std::getline(f, line)) throw std::runtime_error("Empty door csv: " + path);
    auto h = splitCsv(line);
    int ci_f = -1, ci_s = -1, ci_h = -1;
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i] == "frame_id" || h[i] == "frame") ci_f = (int)i;
        else if (h[i] == "state")                  ci_s = (int)i;
        else if (h[i] == "hall_enabled")           ci_h = (int)i;
    }
    if (ci_f < 0 || ci_s < 0) throw std::runtime_error("door csv needs frame_id and state columns: " + path);
    std::map<int64_t, DoorRow> m;
    while (std::getline(f, line)) {
        if (line.empty() || line == "\r") continue;
        auto c = splitCsv(line);
        if ((int)c.size() <= std::max(ci_f, ci_s)) continue;
        DoorRow r; r.state = c[ci_s];
        if (ci_h >= 0 && (int)c.size() > ci_h && !c[ci_h].empty()) {
            const std::string& hv = c[ci_h];
            r.hall = (hv == "1" || hv == "True" || hv == "true") ? 1 : 0;
        }
        m[std::stoll(c[ci_f])] = r;
    }
    return m;
}

// Door state at a frame = the last row whose frame <= this frame (UNKNOWN before the first row).
static DoorRow doorAtFrame(const std::map<int64_t, DoorRow>& m, int64_t frame) {
    auto it = m.upper_bound(frame);
    if (it == m.begin()) return DoorRow{};
    return std::prev(it)->second;
}

// ---- options --------------------------------------------------------------
struct RunOptions { int hall = -1; };   // -1: door csv 의 hall_enabled 를 따른다

static bool applyOption(TrackerConfig& c, ClusterParams& cp, elevsafe::Config& sc,
                        RunOptions& ro, int& min_points, const std::string& kv) {
    auto p = kv.find('=');
    if (p == std::string::npos) return false;
    std::string k = kv.substr(0, p);
    double v = std::stod(kv.substr(p + 1));
    if      (k == "gate_xy")              c.gate_xy = v;
    else if (k == "gate_z")               c.gate_z = v;
    else if (k == "z_weight")             c.z_weight = v;
    else if (k == "n_init")               c.n_init = (int)v;
    else if (k == "door_only_n_init")     c.door_only_n_init = (int)v;
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
    else if (k == "hall")                 ro.hall = (v != 0) ? 1 : 0;
    else if (k == "hall_safety")          sc.hall_safety_enabled = (v != 0);
    else if (k == "door_fall")            sc.door_fall_enabled = (v != 0);
    else if (k == "immobile_sec")         sc.immobile_sec = v;
    else if (k == "close_wait_sec")       sc.close_wait_sec = v;
    else return false;
    return true;
}

static bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

struct IdInfo {
    int64_t first_frame = 0, last_frame = 0;
    int64_t first_ts = 0, last_ts = 0;
    int n_frames = 0;
    int64_t fall_frame = -1, fall_ts = 0;           // 처음 FALL 이 보인 프레임
    int64_t immobile_frame = -1, immobile_ts = 0;   // 처음 IMMOBILE 이 보인 프레임
    std::string fall_zone;                          // 그때의 zone
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <input_clusters.csv> [result.csv] [door_csv=<file> | door=<STATE> [door_close_sec=<s>]] [hall=0|1] [hall_safety=0|1] [gate_xy=0.55 ...]\n";
        return 1;
    }
    std::string in_path = argv[1];
    std::string stem = in_path;
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".csv") == 0) stem.resize(stem.size() - 4);
    std::string out_path   = stem + "_result.csv";
    std::string state_path = stem + "_result_state.csv";
    std::string sum_path   = stem + "_result_summary.txt";
    TrackerConfig cfg;
    ClusterParams cparams;
    elevsafe::Config scfg;
    RunOptions ro;
    int min_points = 21;  // 확정 기본값

    std::string door_csv, door_fixed;
    double door_close_sec = -1.0;

    int i = 2;
    if (argc > 2 && std::string(argv[2]).find('=') == std::string::npos) {
        out_path = argv[2];
        if (out_path.size() > 4 && out_path.compare(out_path.size() - 4, 4, ".csv") == 0) {
            std::string s2 = out_path.substr(0, out_path.size() - 4);
            state_path = s2 + "_state.csv";
            sum_path   = s2 + "_summary.txt";
        }
        i = 3;
    }

    try {
        for (; i < argc; ++i) {
            std::string a = argv[i];
            if      (startsWith(a, "door_csv="))       door_csv = a.substr(9);
            else if (startsWith(a, "door_close_sec=")) door_close_sec = std::stod(a.substr(15));
            else if (startsWith(a, "door="))           door_fixed = a.substr(5);
            else if (!applyOption(cfg, cparams, scfg, ro, min_points, a)) {
                std::cerr << "Unknown option: " << a << "\n";
                return 1;
            }
        }
        if (!door_csv.empty() && !door_fixed.empty()) {
            std::cerr << "Use either door_csv= or door=, not both\n";
            return 1;
        }

        std::cout << "Loading: " << in_path << std::endl;
        auto frames = loadDetectionsFromClustersCsv(in_path, min_points);   // 빈 프레임도 포함
        if (frames.empty()) {
            std::cerr << "No frames in: " << in_path << "\n";
            return 1;
        }

        std::map<int64_t, DoorRow> door_rows;
        std::string door_desc = "none(UNKNOWN)";
        if (!door_csv.empty()) {
            door_rows = loadDoorFile(door_csv);
            door_desc = "csv(" + door_csv + ")";
            std::cout << "Door csv rows: " << door_rows.size() << "\n";
        } else if (!door_fixed.empty()) {
            door_desc = "fixed(" + door_fixed + (door_close_sec >= 0 ? ", CLOSED after " + std::to_string(door_close_sec) + "s" : "") + ")";
        }

        TrackingEngine eng(cfg, cparams, min_points, scfg);
        std::ofstream fo(out_path);
        if (!fo) { std::cerr << "Cannot write result file: " << out_path << "\n"; return 1; }
        std::ofstream fs(state_path);
        if (!fs) { std::cerr << "Cannot write state file: " << state_path << "\n"; return 1; }
        fo << std::fixed << std::setprecision(3);
        fo << "frame,timestamp_us,id,state,x,y,z,vx,vy,vz,n_points,time_since_update_s,zone,phase,safety\n";
        fs << "frame,timestamp_us,door_state,door_command,safety_state,hold_exceeded,door_open_limit_warning,reason,tracks(id:phase:safety:zone)\n";

        std::map<int, IdInfo> ids;
        std::map<std::string, size_t> door_frames;
        std::map<std::string, size_t> danger_by_zone;     // FALL/IMMOBILE 트랙-프레임 수 (zone 별)
        std::map<std::string, size_t> cmd_frames;
        size_t hall_frames = 0, max_conf = 0;
        const int64_t ts0 = frames.front().timestamp_us;

        for (const auto& fd : frames) {
            DoorRow dr;
            if (!door_csv.empty()) dr = doorAtFrame(door_rows, fd.frame);
            else if (!door_fixed.empty()) {
                dr.state = door_fixed;
                if (door_close_sec >= 0 && (fd.timestamp_us - ts0) / 1e6 >= door_close_sec) dr.state = "CLOSED";
            }
            const bool hall = (ro.hall >= 0) ? (ro.hall != 0) : (dr.hall != 0);
            eng.setHallEnabled(hall);
            ++door_frames[dr.state];
            hall_frames += hall ? 1 : 0;

            const elevsafe::DoorState ds = elevsafe::parseDoorState(dr.state);   // OPENING/CLOSING -> OPEN
            const FrameResult r = eng.processFrame(fd, ds);
            ++cmd_frames[elevsafe::toString(r.door_command)];

            size_t conf = 0;
            std::string track_desc;
            for (const TrackOutput& t : r.tracks) {
                const char* zone = elevsafe::toString(t.zone);
                fo << fd.frame << ',' << fd.timestamp_us << ',' << t.id << ','
                   << stateName(t.state) << ',' << t.x << ',' << t.y << ',' << t.z << ','
                   << t.vx << ',' << t.vy << ',' << t.vz << ',' << t.n_points << ','
                   << t.time_since_update_s << ',' << zone << ','
                   << elevsafe::toString(t.phase) << ',' << elevsafe::toString(t.safety) << '\n';

                if (!track_desc.empty()) track_desc += '|';
                track_desc += std::to_string(t.id) + ":" + elevsafe::toString(t.phase) + ":" +
                              elevsafe::toString(t.safety) + ":" + zone;

                if (t.safety != elevsafe::Safety::NORMAL) ++danger_by_zone[zone];

                if (!t.lost) ++conf;
                auto it = ids.find(t.id);
                if (it == ids.end()) {
                    IdInfo info;
                    info.first_frame = fd.frame; info.first_ts = fd.timestamp_us;
                    it = ids.emplace(t.id, info).first;
                }
                IdInfo& info = it->second;
                if (!t.lost) {
                    info.last_frame = fd.frame; info.last_ts = fd.timestamp_us; info.n_frames++;
                }
                if (t.safety != elevsafe::Safety::NORMAL && info.fall_frame < 0) {
                    info.fall_frame = fd.frame; info.fall_ts = fd.timestamp_us; info.fall_zone = zone;
                }
                if (t.safety == elevsafe::Safety::IMMOBILE && info.immobile_frame < 0) {
                    info.immobile_frame = fd.frame; info.immobile_ts = fd.timestamp_us;
                }
            }
            fs << fd.frame << ',' << fd.timestamp_us << ',' << elevsafe::toString(ds) << ','
               << elevsafe::toString(r.door_command) << ',' << elevsafe::toString(r.safety_state) << ','
               << (r.hold_exceeded ? 1 : 0) << ',' << (r.door_open_limit_warning ? 1 : 0) << ','
               << r.reason << ',' << track_desc << '\n';
            max_conf = std::max(max_conf, conf);
        }

        double dur_s = (frames.back().timestamp_us - frames.front().timestamp_us) / 1e6;
        std::ostringstream sum;
        sum << std::fixed << std::setprecision(3);
        sum << "=== Summary ===\n"
            << "Input: " << in_path << "\n"
            << "Result: " << out_path << "\n"
            << "Config: gate_xy=" << cfg.gate_xy << " gate_z=" << cfg.gate_z << " z_weight=" << cfg.z_weight
            << " n_init=" << cfg.n_init << " max_age_s=" << cfg.max_age_s
            << " tentative_max_gap_s=" << cfg.tentative_max_gap_s << " min_points=" << min_points << "\n"
            << "State config: hall_safety=" << (scfg.hall_safety_enabled ? 1 : 0)
            << " immobile_sec=" << scfg.immobile_sec << " close_wait_sec=" << scfg.close_wait_sec << "\n"
            << "Frames: " << frames.size() << "  (about " << dur_s << " s)\n"
            << "Public IDs assigned: " << eng.manager().confirmedIdCount() << "\n"
            << "Max simultaneous (non-LOST) tracks: " << max_conf << "\n"
            << "Door: " << door_desc << "  hall_enabled_frames=" << hall_frames << "\n"
            << "Door state frames (raw):";
        for (const auto& [s, n] : door_frames) sum << " " << s << "=" << n;
        sum << "\nDoor command frames:";
        for (const auto& [s, n] : cmd_frames) sum << " " << s << "=" << n;
        sum << "\nFALL/IMMOBILE track-frames by zone:";
        if (danger_by_zone.empty()) sum << " none";
        for (const auto& [z, n] : danger_by_zone) sum << " " << z << "=" << n;
        sum << "\n\nPer-ID lifetime and safety\n"
            << "  ID  first_frame  last_frame  duration_s  n_frames  first_FALL(frame,zone)  first_IMMOBILE(frame)\n";
        for (const auto& [id, info] : ids) {
            sum << "  " << id << "   " << info.first_frame << "   " << info.last_frame << "   "
                << (info.last_ts - info.first_ts) / 1e6 << "   " << info.n_frames << "   ";
            if (info.fall_frame >= 0) sum << info.fall_frame << "," << info.fall_zone; else sum << "-";
            sum << "   ";
            if (info.immobile_frame >= 0) sum << info.immobile_frame; else sum << "-";
            sum << "\n";
        }
        std::cout << sum.str() << "Saved: " << out_path << ", " << state_path << ", " << sum_path << "\n";
        std::ofstream fsum(sum_path);
        if (fsum) fsum << sum.str();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}