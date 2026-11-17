// tracking/offline_runner.cpp
// Point-cloud CSV -> per-frame track result CSV + summary
// Usage: tracking_offline <input.csv> [result.csv] [gate_xy=0.8 n_init=3 max_age_s=5 ...]
// If the CSV has no cluster/target_id column, points are clustered per frame (DBSCAN):
//   eps_xy=0.5 z_scale=0.5 cluster_min_pts=3 min_points=3
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include "id_manager/id_manager.hpp"

static const char* stateName(TrackState s) {
    switch (s) {
        case TrackState::TENTATIVE: return "TENTATIVE";
        case TrackState::CONFIRMED: return "CONFIRMED";
        case TrackState::LOST:      return "LOST";
    }
    return "?";
}

// Apply a "key=value" argument to TrackerConfig.
static bool applyOption(TrackerConfig& c, ClusterParams& cp, int& min_points, const std::string& kv) {
    auto p = kv.find('=');
    if (p == std::string::npos) return false;
    std::string k = kv.substr(0, p);
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
    else return false;
    return true;
}

struct IdInfo {
    int64_t first_frame = 0, last_frame = 0;
    int64_t first_ts = 0, last_ts = 0;
    int n_frames = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <input.csv> [result.csv] [gate_xy=0.8 n_init=3 max_age_s=5 ...]\n";
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_path = "result.csv";
    TrackerConfig cfg;
    ClusterParams cparams;
    int min_points = 3;

    int i = 2;
    if (argc > 2 && std::string(argv[2]).find('=') == std::string::npos) {
        out_path = argv[2];
        i = 3;
    }

    try {
        for (; i < argc; ++i) {
            if (!applyOption(cfg, cparams, min_points, argv[i])) {
                std::cerr << "Unknown option: " << argv[i] << "\n";
                return 1;
            }
        }

        std::cout << "Loading: " << in_path << std::endl;
        auto frames = loadDetectionsFromPointCloudCsv(in_path, min_points, cparams);
        if (frames.empty()) {
            std::cerr << "No frames with detections in: " << in_path << "\n";
            return 1;
        }

        TrackManager tm(cfg);
        std::ofstream fo(out_path);
        if (!fo) { std::cerr << "Cannot write result file: " << out_path << "\n"; return 1; }
        fo << std::fixed << std::setprecision(3);
        fo << "frame,timestamp_us,internal_id,id,state,x,y,z,vx,vy,vz,n_points,time_since_update_s\n";

        std::map<int, IdInfo> ids;
        size_t max_conf = 0;
        size_t removed_total = 0;

        for (const auto& fd : frames) {
            const auto& tracks = tm.update(fd);
            removed_total += tm.removedTracks().size();

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

        double dur_s = (frames.back().timestamp_us - frames.front().timestamp_us) / 1e6;
        std::cout << "=== Summary ===\n"
                  << "Input: " << in_path << "\n"
                  << "Frames with detections: " << frames.size() << "  (about " << dur_s << " s)\n"
                  << "Public IDs assigned: " << tm.confirmedIdCount() << "\n"
                  << "Max simultaneous CONFIRMED: " << max_conf << "\n"
                  << "Tracks removed: " << removed_total << "\n"
                  << "Saved: " << out_path << "\n\n"
                  << "Per-ID lifetime (CONFIRMED only)\n"
                  << "  ID  first_frame  last_frame  duration_s  n_frames\n";
        for (const auto& [id, info] : ids) {
            std::cout << "  " << id << "   " << info.first_frame << "   " << info.last_frame << "   "
                      << (info.last_ts - info.first_ts) / 1e6 << "   " << info.n_frames << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
