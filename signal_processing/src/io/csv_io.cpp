#include "io/csv_io.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <algorithm>

namespace {
std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// 한 줄을 ',' 기준으로 잘라 칸 목록으로 (각 칸은 앞뒤 공백 제거)
std::vector<std::string> splitLine(const std::string& line) {
    std::vector<std::string> cells;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) cells.push_back(trim(cell));
    return cells;
}
}  // namespace

std::vector<FrameData> readFramesCSV(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[CSV] 파일을 열 수 없음: " << path << "\n";
        return {};
    }

    // 1. 헤더 읽기: 컬럼 이름 -> 몇 번째 칸인지
    std::string line;
    if (!std::getline(file, line)) {
        std::cerr << "[CSV] 빈 파일: " << path << "\n";
        return {};
    }
    std::unordered_map<std::string, int> col;
    const auto header = splitLine(line);
    for (int i = 0; i < static_cast<int>(header.size()); ++i) col[header[i]] = i;

    // 2. 필수 컬럼 확인
    for (const char* name : {"frame", "x", "y", "z", "doppler", "power"}) {
        // TODO: col 에 name 이 없으면 에러 메시지 출력 후 return {};
        if(col.count(name)==0){
            std::cerr <<"[CSV] 필수 컬럼 없음" << name << " (" << path << ")\n";
            return {};
        }
    }

    // 3. 컬럼 위치 꺼내두기 (선택 컬럼은 없으면 -1)
    const int i_frame = col["frame"];
    const int i_x = col["x"], i_y = col["y"], i_z = col["z"];
    const int i_dop = col["doppler"], i_pow = col["power"];
    const int i_ts  = col.count("timestamp_ms") ? col["timestamp_ms"] : -1;
    const int i_tid = col.count("target_id")    ? col["target_id"]    : -1;

    // 한 줄에 최소 몇 칸이 있어야 필수 값을 다 읽을 수 있는지
    int need = 0;
    for (int i : {i_frame, i_x, i_y, i_z, i_dop, i_pow}) need = std::max(need, i + 1);

    // 4. 데이터 읽기
    std::map<int, FrameData> by_frame;
    int line_no = 1;
    while (std::getline(file, line)) {
        ++line_no;
        const auto cells = splitLine(line);
        if (cells.empty() || (cells.size() == 1 && cells[0].empty())) continue;

        if (static_cast<int>(cells.size()) < need) {
            std::cerr << "[CSV] " << path << ":" << line_no << " 칸 수 부족, 건너뜀\n";
            continue;
        }
        try {
            const int frame = std::stoi(cells[i_frame]);

            RadarPoint p;
            // cells[i_x] ... cells[i_pow] 를 std::stof 로 읽어 p.x, p.y, p.z, p.doppler, p.power 에 넣기
            // i_tid 가 0 이상이고 그 칸이 존재하면 p.target_id = std::stoi(...)
            p.x = std::stof(cells[i_x]);
            p.y = std::stof(cells[i_y]);
            p.z = std::stof(cells[i_z]);
            p.doppler = std::stof(cells[i_dop]);
            p.power = std::stof(cells[i_pow]); 
            if(i_tid>=0 && i_tid<static_cast<int>(cells.size())){
                p.target_id = std::stoi(cells[i_tid]);
            }

            FrameData& fd = by_frame[frame];   // 처음 보는 frame 이면 새로 생성됨
            fd.frame_id = frame;
            // i_ts 가 0 이상이면 fd.timestamp_ms = std::stod(cells[i_ts]);
            if(i_ts>=0) fd.timestamp_ms = std::stod(cells[i_ts]);
            fd.points.push_back(p);
        } catch (...) {
            std::cerr << "[CSV] " << path << ":" << line_no << " 숫자 변환 실패, 건너뜀\n";
        }
    }

    std::vector<FrameData> frames;
    frames.reserve(by_frame.size());
    for (auto& kv : by_frame) frames.push_back(std::move(kv.second));
    return frames;
}

bool writeFramesCSV(const std::string& path, const std::vector<FrameData>& frames) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "[CSV] 파일을 쓸 수 없음: " << path << "\n";
        return false;
    }

    file << std::fixed << std::setprecision(3);   // 소수점 3자리 (m 기준 mm, ms 기준 µs)
    file << "frame,timestamp_ms,x,y,z,doppler,power,target_id,zone,cluster\n";
    for (const auto& fd : frames) {
        for (const auto& p : fd.points) {
            // fd.frame_id, fd.timestamp_ms, p.x, p.y, p.z, p.doppler, p.power,
            // p.target_id, zoneToString(p.zone) 를 ',' 로 이어서 한 줄 쓰기
            file << fd.frame_id << ','<< fd.timestamp_ms << ','
            << p.x << ',' << p.y << ',' << p.z << ','
            << p.doppler << ',' << p.power << ','
            << p.target_id << ',' << zoneToString(p.zone) << ','<<p.cluster_id<< '\n';
        }
    }
    return true;
}