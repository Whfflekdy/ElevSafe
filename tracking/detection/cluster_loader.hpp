// cluster_loader.hpp
// B(신호처리)가 준 *_clusters.csv / *_door.csv 로더 (header-only, C++17)
//
// clusters.csv 열:
//   frame_id,timestamp_us,cluster_id,cx,cy,cz,x_min,x_max,y_min,y_max,
//   z_min,z_max,z_top,mean_doppler,num_points,zone,maybe_merged
//   (+ 새 버전: doppler_std,power_sum,z_p10,z_p50 — 있으면 읽고 없으면 0)
// - 클러스터가 없는 프레임은 "frame_id,timestamp_us" 두 칸만 있는 줄이다.
//   이런 프레임도 "검출 0개" ClusterFrame으로 남긴다 (트래커 LOST 처리/시간 흐름 유지용).
// - 시간 단위는 마이크로초(us). 기존 timestamp_ms 로더와 다르니 주의.
//
// door.csv 열: frame_id,timestamp_us,state[,hall_enabled]   (OPEN / CLOSED / UNKNOWN, hall_enabled 0/1)
// - 두 파일의 timestamp_us 가 서로 다를 수 있다(S0-6: 약 120초 vs 100초). 프레임 매칭은 frame_id 로 한다.
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "elevsafe_types.hpp"

namespace elevsafe {

// ---------------------------------------------------------------- 공통 유틸
namespace detail {

inline std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    std::stringstream ss(line);
    while (std::getline(ss, cell, ',')) {
        // 개행(\r) 제거 + 앞뒤 공백 제거
        while (!cell.empty() && (cell.back() == '\r' || cell.back() == ' ')) cell.pop_back();
        size_t b = 0;
        while (b < cell.size() && cell[b] == ' ') ++b;
        out.push_back(cell.substr(b));
    }
    return out;
}

inline std::unordered_map<std::string, size_t> headerIndex(const std::string& headerLine) {
    std::unordered_map<std::string, size_t> idx;
    auto cols = splitCsvLine(headerLine);
    for (size_t i = 0; i < cols.size(); ++i) idx[cols[i]] = i;
    return idx;
}

inline size_t requireCol(const std::unordered_map<std::string, size_t>& idx,
                         const std::string& name, const std::string& path) {
    auto it = idx.find(name);
    if (it == idx.end())
        throw std::runtime_error(path + ": 필요한 열 '" + name + "' 이(가) 없음");
    return it->second;
}

}  // namespace detail

// ---------------------------------------------------------------- 클러스터
// DetZone / parseZone 은 elevsafe_types.hpp 로 옮겼다 (Detection.zone 과 같은 타입).

struct ClusterRecord {
    int cluster_id = 0;
    double cx = 0, cy = 0, cz = 0;
    double x_min = 0, x_max = 0, y_min = 0, y_max = 0, z_min = 0, z_max = 0;
    double z_top = 0;          // 클러스터 최상단 높이 (FALL 판정 등에 사용)
    double mean_doppler = 0;
    int num_points = 0;
    std::string zone;          // CABIN / DOOR / HALL 원문 문자열 (기존 코드 호환)
    DetZone det_zone = DetZone::UNKNOWN;  // zone 문자열을 enum 으로 바꾼 값 (Detection.zone 에 넣을 것)
    double doppler_std = 0, power_sum = 0, z_p10 = 0, z_p50 = 0;  // 새 CSV 열 (없으면 0)
    bool maybe_merged = false; // 두 사람이 한 덩어리로 잡혔을 가능성
};

struct ClusterFrame {
    int64_t frame_id = 0;
    int64_t timestamp_us = 0;
    std::vector<ClusterRecord> clusters;  // 비어 있을 수 있음
    double timestamp_ms() const { return timestamp_us / 1000.0; }
};

// 프레임 순서(파일 등장 순서) 그대로 반환. 같은 frame_id의 줄은 한 프레임으로 묶는다.
inline std::vector<ClusterFrame> loadClusterCsv(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("파일을 열 수 없음: " + path);

    std::string line;
    if (!std::getline(f, line)) throw std::runtime_error(path + ": 빈 파일");
    auto idx = detail::headerIndex(line);

    const size_t cFrame = detail::requireCol(idx, "frame_id", path);
    const size_t cTime  = detail::requireCol(idx, "timestamp_us", path);
    const size_t cId    = detail::requireCol(idx, "cluster_id", path);
    // 나머지 열은 있으면 읽고 없으면 기본값
    auto opt = [&](const char* n) -> long {
        auto it = idx.find(n);
        return it == idx.end() ? -1 : static_cast<long>(it->second);
    };
    const long cCx = opt("cx"), cCy = opt("cy"), cCz = opt("cz");
    const long cXmin = opt("x_min"), cXmax = opt("x_max");
    const long cYmin = opt("y_min"), cYmax = opt("y_max");
    const long cZmin = opt("z_min"), cZmax = opt("z_max"), cZtop = opt("z_top");
    const long cDop = opt("mean_doppler"), cNum = opt("num_points");
    const long cZone = opt("zone"), cMerged = opt("maybe_merged");
    const long cDopStd = opt("doppler_std"), cPow = opt("power_sum");
    const long cZp10 = opt("z_p10"), cZp50 = opt("z_p50");

    std::vector<ClusterFrame> frames;
    std::unordered_map<int64_t, size_t> frameAt;  // frame_id -> frames 인덱스

    while (std::getline(f, line)) {
        if (line.empty() || line == "\r") continue;
        auto v = detail::splitCsvLine(line);
        if (v.size() <= cTime) continue;  // 깨진 줄은 건너뜀

        const int64_t fid = std::stoll(v[cFrame]);
        auto it = frameAt.find(fid);
        if (it == frameAt.end()) {
            ClusterFrame fr;
            fr.frame_id = fid;
            fr.timestamp_us = std::stoll(v[cTime]);
            frames.push_back(std::move(fr));
            it = frameAt.emplace(fid, frames.size() - 1).first;
        }

        // 클러스터 정보가 없는 "빈 프레임" 줄이면 여기서 끝
        if (v.size() <= cId || v[cId].empty()) continue;

        auto num = [&](long c) -> double {
            return (c >= 0 && static_cast<size_t>(c) < v.size() && !v[c].empty()) ? std::stod(v[c]) : 0.0;
        };
        ClusterRecord r;
        r.cluster_id = std::stoi(v[cId]);
        r.cx = num(cCx); r.cy = num(cCy); r.cz = num(cCz);
        r.x_min = num(cXmin); r.x_max = num(cXmax);
        r.y_min = num(cYmin); r.y_max = num(cYmax);
        r.z_min = num(cZmin); r.z_max = num(cZmax); r.z_top = num(cZtop);
        r.mean_doppler = num(cDop);
        r.num_points = static_cast<int>(num(cNum));
        r.doppler_std = num(cDopStd); r.power_sum = num(cPow);
        r.z_p10 = num(cZp10); r.z_p50 = num(cZp50);
        if (cZone >= 0 && static_cast<size_t>(cZone) < v.size()) {
            r.zone = v[cZone];
            r.det_zone = parseZone(r.zone);
        }
        if (cMerged >= 0 && static_cast<size_t>(cMerged) < v.size())
            r.maybe_merged = (v[cMerged] == "1" || v[cMerged] == "True" || v[cMerged] == "true");
        frames[it->second].clusters.push_back(std::move(r));
    }
    return frames;
}

// ---------------------------------------------------------------- 문 상태
// DoorState / parseDoorState / toString 도 elevsafe_types.hpp 로 옮겼다 (state_machine.hpp 와 중복 정의 방지).

// frame_id -> DoorState
inline std::map<int64_t, DoorState> loadDoorCsv(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("파일을 열 수 없음: " + path);

    std::string line;
    if (!std::getline(f, line)) throw std::runtime_error(path + ": 빈 파일");
    auto idx = detail::headerIndex(line);
    const size_t cFrame = detail::requireCol(idx, "frame_id", path);
    const size_t cState = detail::requireCol(idx, "state", path);

    std::map<int64_t, DoorState> out;
    while (std::getline(f, line)) {
        if (line.empty() || line == "\r") continue;
        auto v = detail::splitCsvLine(line);
        if (v.size() <= cState) continue;
        out[std::stoll(v[cFrame])] = parseDoorState(v[cState]);
    }
    return out;
}

// door.csv 한 줄 전체 (state + 프레임 단위 hall_enabled + 시간)
struct DoorInfo {
    DoorState state = DoorState::UNKNOWN;
    bool hall_enabled = false;   // hall_enabled 열이 없으면 false
    int64_t timestamp_us = 0;
};

// frame_id -> DoorInfo. hall_enabled 가 필요하면 loadDoorCsv 대신 이걸 쓴다.
inline std::map<int64_t, DoorInfo> loadDoorInfoCsv(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("파일을 열 수 없음: " + path);

    std::string line;
    if (!std::getline(f, line)) throw std::runtime_error(path + ": 빈 파일");
    auto idx = detail::headerIndex(line);
    const size_t cFrame = detail::requireCol(idx, "frame_id", path);
    const size_t cState = detail::requireCol(idx, "state", path);
    auto oc = [&](const char* n) -> long {
        auto it = idx.find(n);
        return it == idx.end() ? -1 : static_cast<long>(it->second);
    };
    const long cTime = oc("timestamp_us"), cHall = oc("hall_enabled");

    std::map<int64_t, DoorInfo> out;
    while (std::getline(f, line)) {
        if (line.empty() || line == "\r") continue;
        auto v = detail::splitCsvLine(line);
        if (v.size() <= cState) continue;
        DoorInfo d;
        d.state = parseDoorState(v[cState]);
        if (cHall >= 0 && static_cast<size_t>(cHall) < v.size())
            d.hall_enabled = (v[cHall] == "1" || v[cHall] == "True" || v[cHall] == "true");
        if (cTime >= 0 && static_cast<size_t>(cTime) < v.size() && !v[cTime].empty())
            d.timestamp_us = std::stoll(v[cTime]);
        out[std::stoll(v[cFrame])] = d;
    }
    return out;
}

inline DoorInfo doorInfoAt(const std::map<int64_t, DoorInfo>& doors, int64_t frame_id) {
    auto it = doors.find(frame_id);
    return it == doors.end() ? DoorInfo{} : it->second;
}

// 프레임 번호로 조회. 없으면 UNKNOWN.
inline DoorState doorAt(const std::map<int64_t, DoorState>& doors, int64_t frame_id) {
    auto it = doors.find(frame_id);
    return it == doors.end() ? DoorState::UNKNOWN : it->second;
}

}  // namespace elevsafe
