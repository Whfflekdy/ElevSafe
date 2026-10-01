#include "io/csv_io.h"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

std::vector<Frame> readFramesCSV(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[CSV] 파일을 열 수 없음: " << path << "\n";
        return {};
    }

    std::map<int, Frame> by_frame;   // frame 번호 -> 그 프레임의 포인트들 (번호 순 자동 정렬)
    std::string line;
    std::getline(file, line);        // 첫 줄(헤더)은 건너뜀

    int line_no = 1;
    while (std::getline(file, line)) {
        ++line_no;
        if (line.empty()) continue;

        // 한 줄을 ',' 기준으로 잘라 숫자 7개를 v[]에 담기
        std::stringstream ss(line);
        std::string cell;
        float v[7];
        int n = 0;
        try {
            while (n < 7 && std::getline(ss, cell, ',')) {
                v[n++] = std::stof(cell);
            }
        } catch (...) {
            n = -1;   // 숫자가 아닌 칸이 있으면 실패 표시
        }
        if (n < 6) {
            std::cerr << "[CSV] " << path << ":" << line_no << " 파싱 실패, 건너뜀\n";
            continue;
        }

        // RadarPoint p 를 만들어 v[1]~v[5] 를 x, y, z, doppler, power 에 넣기
        RadarPoint p;
        p.x = v[1];
        p.y = v[2];
        p.z = v[3];
        p.doppler = v[4];
        p.power = v[5];
        // target_id 는 칸이 7개(n >= 7)면 v[6], 아니면 기본값인 -1 유지
        if(n>=7) p.target_id = static_cast<int>(v[6]);
        // by_frame[frame 번호] 에 p 추가 (frame 번호 = v[0] 을 int로)
        by_frame[static_cast<int>(v[0])].push_back(p);
    }

    // map 을 vector 로 옮기기 (번호 순서 유지)
    std::vector<Frame> frames;
    frames.reserve(by_frame.size());
    for (auto& kv : by_frame) {
        frames.push_back(std::move(kv.second));
    }
    return frames;
}

// 주의: 출력 frame 열은 원본 frame 번호가 아니라 0부터 매긴 순번이다.
bool writeFramesCSV(const std::string& path, const std::vector<Frame>& frames) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "[CSV] 파일을 쓸 수 없음: " << path << "\n";
        return false;
    }

    file << "frame,x,y,z,doppler,power,target_id,zone\n";
    for (std::size_t f = 0; f < frames.size(); ++f) {
        for (const auto& p : frames[f]) {
            // TODO 4: f, x, y, z, doppler, power, target_id, zone 을 ',' 로 이어 한 줄 쓰기
            //         zone 은 zoneToString(p.zone) 사용, 줄 끝에 '\n'
            file << f << ',' << p.x << ',' << p.y << ',' << p.z << ',' 
            << p.doppler << ',' << p.power << ',' << p.target_id << ',' 
            << zoneToString(p.zone) << '\n';
        }
    }
    return true;
}