// tracking/detection/detection_loader.hpp
// 포인트클라우드를 프레임별 Detection(Bounding Box) 리스트로 변환한다.
//
// [함수 호출용] detectionsFromPoints(): 한 프레임의 점들(PointIn) -> Detection 리스트 (DBSCAN으로 직접 묶음)
// [CSV용]       loadDetectionsFromPointCloudCsv(): CSV 파일 전체 -> FrameDetections 리스트
//
// CSV 열은 헤더 이름으로 찾는다 (순서 무관).
//   필수: frame, x, y, z, doppler, power, (timestamp_us 또는 timestamp_ms)
//   묶음 번호: cluster 열이 있으면 그것을, 없으면 target_id 열을 사용한다.
//     cluster < 0 이거나 250 이상(레이더의 253=미할당, 254=영역 밖, 255=노이즈)인 점은 버린다.
//   cluster/target_id 열이 둘 다 없으면, 프레임마다 점들을 직접 묶는다(DBSCAN).
//     이때 cluster 번호는 그 프레임 안에서만 의미가 있다 (프레임 간 연속성 없음).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Detection {
    int     cluster = -1;
    double  cx = 0, cy = 0, cz = 0;      // 중심 (power 가중 평균) [m]
    double  sx = 0, sy = 0, sz = 0;      // 크기 (max-min) [m]
    double  zmin = 0, zmax = 0;          // Z축 범위 [m]
    int     n_points = 0;
    double  mean_doppler = 0;
    double  mean_power = 0;
};

struct FrameDetections {
    int64_t frame = 0;
    int64_t timestamp_us = 0;            // dt는 프레임 번호가 아니라 이 값으로 계산할 것
    std::vector<Detection> detections;
};

// 입력 점 하나. doppler/power를 모르면 기본값(0, 1)으로 둬도 된다 (power는 중심 계산 가중치).
struct PointIn {
    double x = 0, y = 0, z = 0;
    double doppler = 0;
    double power = 1;
};

// 직접 묶기(DBSCAN) 설정. 임시값이므로 사람이 탄 녹화본으로 정할 것.
struct ClusterParams {
    double eps_xy = 0.5;      // [m] 수평 이웃 거리
    double z_scale = 0.5;     // z 차이에 곱하는 가중치 (작을수록 수직 방향으로 더 멀리 이어붙임)
    int    min_pts = 3;       // 핵심점 판정에 필요한 이웃 수(자기 포함)
};

namespace detail {

// 한 프레임의 점들을 DBSCAN으로 묶는다. 반환: 점마다 라벨(0..k-1), 노이즈는 -1.
inline std::vector<int> dbscan(const std::vector<PointIn>& p, const ClusterParams& cp) {
    const int n = (int)p.size();
    std::vector<int> label(n, -2);  // -2 = 미방문, -1 = 노이즈
    auto near = [&](int a, int b) {
        double dx = p[a].x - p[b].x, dy = p[a].y - p[b].y, dz = (p[a].z - p[b].z) * cp.z_scale;
        return std::sqrt(dx * dx + dy * dy + dz * dz) <= cp.eps_xy;
    };
    auto neighbors = [&](int a) {
        std::vector<int> r;
        for (int b = 0; b < n; ++b) if (near(a, b)) r.push_back(b);
        return r;
    };
    int cid = 0;
    for (int i = 0; i < n; ++i) {
        if (label[i] != -2) continue;
        auto nb = neighbors(i);
        if ((int)nb.size() < cp.min_pts) { label[i] = -1; continue; }
        label[i] = cid;
        std::vector<int> queue;
        for (int k : nb) queue.push_back(k);
        for (size_t q = 0; q < queue.size(); ++q) {
            int j = queue[q];
            if (label[j] == -1) label[j] = cid;      // 노이즈였던 점은 경계점으로 편입
            if (label[j] != -2) continue;
            label[j] = cid;
            auto nb2 = neighbors(j);
            if ((int)nb2.size() >= cp.min_pts) for (int k : nb2) queue.push_back(k);
        }
        ++cid;
    }
    return label;
}

}  // namespace detail

// 점 묶음 하나 -> Detection (중심은 power 가중 평균, 크기는 max-min)
inline Detection makeDetection(int cluster_id, const std::vector<PointIn>& pts) {
    Detection d;
    d.cluster = cluster_id;
    d.n_points = (int)pts.size();
    double wsum = 0, xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9, zmin = 1e9, zmax = -1e9;
    for (const PointIn& p : pts) wsum += p.power;
    const bool weighted = wsum > 0;               // power가 전부 0이면 동일 가중치로 계산
    if (!weighted) wsum = (double)pts.size();
    for (const PointIn& p : pts) {
        const double w = weighted ? p.power : 1.0;
        d.cx += p.x * w; d.cy += p.y * w; d.cz += p.z * w;
        d.mean_doppler += p.doppler; d.mean_power += p.power;
        xmin = std::min(xmin, p.x); xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y);
        zmin = std::min(zmin, p.z); zmax = std::max(zmax, p.z);
    }
    d.cx /= wsum; d.cy /= wsum; d.cz /= wsum;
    d.mean_doppler /= d.n_points; d.mean_power /= d.n_points;
    d.sx = xmax - xmin; d.sy = ymax - ymin; d.sz = zmax - zmin;
    d.zmin = zmin; d.zmax = zmax;
    return d;
}

// [함수 호출용] 한 프레임의 점들 -> Detection 리스트. 점이 min_points 미만인 묶음은 버린다.
inline std::vector<Detection> detectionsFromPoints(
        const std::vector<PointIn>& points, int min_points = 3,
        const ClusterParams& cluster_params = ClusterParams()) {
    std::vector<Detection> out;
    if (points.empty()) return out;
    auto label = detail::dbscan(points, cluster_params);
    std::map<int, std::vector<PointIn>> groups;
    for (size_t i = 0; i < points.size(); ++i)
        if (label[i] >= 0) groups[label[i]].push_back(points[i]);
    for (auto& [cid, pts] : groups)
        if ((int)pts.size() >= min_points) out.push_back(makeDetection(cid, pts));
    return out;
}

inline std::vector<FrameDetections> loadDetectionsFromPointCloudCsv(
        const std::string& path, int min_points = 3,
        const ClusterParams& cluster_params = ClusterParams()) {
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
    {
        auto h = split(line);
        for (int i = 0; i < (int)h.size(); ++i) col[h[i]] = i;
    }
    auto need = [&](const char* name) {
        auto it = col.find(name);
        if (it == col.end()) throw std::runtime_error(std::string("missing column: ") + name);
        return it->second;
    };
    const int c_frame = need("frame"), c_x = need("x"), c_y = need("y"), c_z = need("z");
    const int c_dop = need("doppler"), c_pow = need("power");
    int c_ts = -1; double ts_scale = 1.0;                       // 값 * ts_scale = us
    if (col.count("timestamp_us"))      { c_ts = col["timestamp_us"]; ts_scale = 1.0; }
    else if (col.count("timestamp_ms")) { c_ts = col["timestamp_ms"]; ts_scale = 1000.0; }
    else throw std::runtime_error("missing column: timestamp_us or timestamp_ms");
    int c_cl = -1;                                              // -1이면 자체 묶기
    if (col.count("cluster"))        c_cl = col["cluster"];
    else if (col.count("target_id")) c_cl = col["target_id"];

    int max_col = std::max({c_frame, c_x, c_y, c_z, c_dop, c_pow, c_ts});
    if (c_cl >= 0) max_col = std::max(max_col, c_cl);

    struct Acc { int64_t ts = 0; std::vector<PointIn> pts; };
    std::map<int64_t, std::map<int, Acc>> grouped;   // frame -> cluster -> 점들
    std::map<int64_t, Acc> raw_frames;               // 자체 묶기용: frame -> 점들

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto f = split(line);
        if ((int)f.size() <= max_col) continue;  // 깨진 행 무시
        int64_t frame;
        int64_t ts;
        PointIn pt;
        try {
            frame = std::stoll(f[c_frame]);
            ts = (int64_t)std::llround(std::stod(f[c_ts]) * ts_scale);
            pt.x = std::stod(f[c_x]); pt.y = std::stod(f[c_y]); pt.z = std::stod(f[c_z]);
            pt.doppler = std::stod(f[c_dop]); pt.power = std::stod(f[c_pow]);
        } catch (...) { continue; }
        if (c_cl >= 0) {
            int cluster;
            try { cluster = std::stoi(f[c_cl]); } catch (...) { continue; }
            if (cluster < 0 || cluster >= 250) continue;  // 노이즈/미할당
            Acc& a = grouped[frame][cluster];
            a.ts = ts;
            a.pts.push_back(pt);
        } else {
            Acc& a = raw_frames[frame];
            a.ts = ts;
            a.pts.push_back(pt);
        }
    }

    if (c_cl < 0) {  // 프레임마다 직접 묶기
        for (auto& [frame, a] : raw_frames) {
            auto label = detail::dbscan(a.pts, cluster_params);
            for (size_t i = 0; i < a.pts.size(); ++i) {
                if (label[i] < 0) continue;
                Acc& g = grouped[frame][label[i]];
                g.ts = a.ts;
                g.pts.push_back(a.pts[i]);
            }
        }
    }

    std::vector<FrameDetections> out;
    for (auto& [frame, clusters] : grouped) {
        FrameDetections fd;
        fd.frame = frame;
        for (auto& [cid, a] : clusters) {
            if ((int)a.pts.size() < min_points) continue;
            fd.timestamp_us = a.ts;
            fd.detections.push_back(makeDetection(cid, a.pts));
        }
        if (!fd.detections.empty()) out.push_back(std::move(fd));
    }
    return out;
}
