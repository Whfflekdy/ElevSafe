// tracking/detection/cluster_csv_loader.hpp
// B님이 정제한 "클러스터 단위 CSV"(*_clusters.csv)를 읽어 프레임별 Detection 리스트로 변환한다.
// 점 단위 CSV는 detection_loader.hpp의 loadDetectionsFromPointCloudCsv()를 쓴다.
//
// 이미 클러스터링이 끝난 데이터이므로 DBSCAN은 쓰지 않는다. 클러스터 한 줄 = Detection 하나.
//
// CSV 형식 (헤더 이름으로 열을 찾는다, 순서 무관)
//   frame_id,timestamp_us,cluster_id,cx,cy,cz,x_min,x_max,y_min,y_max,z_min,z_max,
//   z_top,mean_doppler,num_points,zone,maybe_merged
//   - 클러스터가 있는 프레임: 위 열이 전부 채워진 행 (클러스터마다 한 줄)
//   - 클러스터가 없는 프레임: "frame_id,timestamp_us" 두 칸만 있는 행 -> 검출 0개 프레임
//
// keep_empty_frames=true(기본): 검출 0개 프레임도 결과에 포함한다.
//   추적기가 그 프레임에서 predict만 하고 트랙 age를 올려야 LOST/DELETED가 제때 동작한다.
//   offline_runner가 detections가 비어 있어도 update를 호출하는지 확인할 것.
#pragma once
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "detection_loader.hpp"   // Detection, FrameDetections (zone/z_top/maybe_merged 필드 필요)

inline std::vector<FrameDetections> loadDetectionsFromClustersCsv(
        const std::string& path, int min_points = 1, bool keep_empty_frames = true) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open: " + path);

    auto split = [](const std::string& line) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && (tok.back() == '\r' || tok.back() == ' ')) tok.pop_back();
            f.push_back(tok);
        }
        return f;
    };

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("empty file: " + path);
    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);  // BOM 제거
    std::map<std::string, int> col;
    int header_size = 0;
    {
        auto h = split(line);
        header_size = (int)h.size();
        for (int i = 0; i < header_size; ++i) col[h[i]] = i;
    }
    auto need = [&](const char* name) {
        auto it = col.find(name);
        if (it == col.end()) throw std::runtime_error(std::string("missing column: ") + name);
        return it->second;
    };
    auto opt = [&](const char* name) {
        auto it = col.find(name);
        return it == col.end() ? -1 : it->second;
    };
    const int c_frame = need("frame_id"), c_ts = need("timestamp_us"), c_id = need("cluster_id");
    const int c_cx = need("cx"), c_cy = need("cy"), c_cz = need("cz"), c_np = need("num_points");
    const int c_xmin = opt("x_min"), c_xmax = opt("x_max"), c_ymin = opt("y_min"), c_ymax = opt("y_max");
    const int c_zmin = opt("z_min"), c_zmax = opt("z_max"), c_ztop = opt("z_top");
    const int c_dop = opt("mean_doppler"), c_zone = opt("zone"), c_mm = opt("maybe_merged");

    std::map<int64_t, FrameDetections> frames;   // frame -> 결과 (프레임 번호 순)

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto f = split(line);
        if (f.size() < 2) continue;

        int64_t frame, ts;
        try {
            frame = std::stoll(f[c_frame]);
            ts = (int64_t)std::llround(std::stod(f[c_ts]));
        } catch (...) { continue; }

        const bool no_cluster_row = (f.size() == 2);          // 빈 프레임 행
        if (!no_cluster_row && (int)f.size() < header_size) continue;  // 깨진 행 무시

        FrameDetections& fd = frames[frame];
        fd.frame = frame;
        fd.timestamp_us = ts;
        if (no_cluster_row) continue;

        auto num = [&](int c, double def = 0.0) {
            if (c < 0) return def;
            try { return std::stod(f[c]); } catch (...) { return def; }
        };
        Detection d;
        try { d.cluster = std::stoi(f[c_id]); } catch (...) { continue; }
        d.n_points = (int)std::llround(num(c_np));
        if (d.n_points < min_points) continue;
        d.cx = num(c_cx); d.cy = num(c_cy); d.cz = num(c_cz);
        if (c_xmin >= 0 && c_xmax >= 0) d.sx = num(c_xmax) - num(c_xmin);
        if (c_ymin >= 0 && c_ymax >= 0) d.sy = num(c_ymax) - num(c_ymin);
        if (c_zmin >= 0 && c_zmax >= 0) { d.zmin = num(c_zmin); d.zmax = num(c_zmax); d.sz = d.zmax - d.zmin; }
        d.z_top = (c_ztop >= 0) ? num(c_ztop) : d.zmax;
        d.mean_doppler = num(c_dop);
        d.mean_power = 0;                                     // 클러스터 CSV에는 power가 없다
        d.maybe_merged = (int)std::llround(num(c_mm));
        if (c_zone >= 0) d.zone = elevsafe::parseZone(f[c_zone]);
        fd.detections.push_back(d);
    }

    std::vector<FrameDetections> out;
    for (auto& [frame, fd] : frames) {
        if (fd.detections.empty() && !keep_empty_frames) continue;
        out.push_back(std::move(fd));
    }
    return out;
}
