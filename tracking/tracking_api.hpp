// tracking/tracking_api.hpp
// 백엔드가 함수 호출로 쓰는 추적 모듈의 입구.
//   입력: 프레임마다 (frame 번호, 타임스탬프[us], 점 목록)
//   출력: 그 프레임 시점의 트랙 목록 (공개 ID, 상태, 위치, 속도)
//
// 사용 예:
//   TrackingEngine engine;                              // 기본 설정. 필요하면 TrackerConfig를 넘긴다.
//   std::vector<PointIn> pts = { {x, y, z, doppler, power}, ... };
//   auto tracks = engine.processPoints(frame, timestamp_us, pts);
//   for (const auto& t : tracks) { /* t.id, t.x, t.y, t.z, t.vx, ... */ }
//
// 점이 하나도 없는 프레임도 반드시 호출할 것. 시간이 흘렀다는 정보가 트랙 생명주기에 필요하다.
#pragma once
#include <cstdint>
#include <vector>
#include "id_manager/id_manager.hpp"
#include "detection/detection_loader.hpp"

struct TrackOutput {
    int         id = -1;                          // 공개 ID (CONFIRMED가 되면 부여, 그 전에는 -1)
    TrackState  state = TrackState::TENTATIVE;
    double      x = 0, y = 0, z = 0;              // 위치 [m]
    double      vx = 0, vy = 0, vz = 0;           // 속도 [m/s]
    int         n_points = 0;                     // 마지막 매칭 검출의 점 개수
    double      time_since_update_s = 0;          // 마지막으로 검출과 매칭된 뒤 경과 시간 [s]
};

class TrackingEngine {
public:
    explicit TrackingEngine(const TrackerConfig& cfg = TrackerConfig(),
                            const ClusterParams& cluster = ClusterParams(),
                            int min_points = 3)
        : tm_(cfg), cluster_(cluster), min_points_(min_points) {}

    // 원시 점으로 호출 (점을 직접 묶는다)
    std::vector<TrackOutput> processPoints(int64_t frame, int64_t timestamp_us,
                                           const std::vector<PointIn>& points,
                                           bool confirmed_only = true) {
        FrameDetections fd;
        fd.frame = frame;
        fd.timestamp_us = timestamp_us;
        fd.detections = detectionsFromPoints(points, min_points_, cluster_);
        return processDetections(fd, confirmed_only);
    }

    // 이미 묶은 검출로 호출 (다른 모듈이 사람 단위로 묶어서 줄 때)
    std::vector<TrackOutput> processDetections(const FrameDetections& fd, bool confirmed_only = true) {
        const std::vector<Track>& tracks = tm_.update(fd);
        std::vector<TrackOutput> out;
        for (const Track& t : tracks) {
            if (confirmed_only && t.state != TrackState::CONFIRMED) continue;
            TrackOutput o;
            o.id = t.id;
            o.state = t.state;
            o.x = t.x(); o.y = t.y(); o.z = t.z();
            o.vx = t.vx(); o.vy = t.vy(); o.vz = t.vz();
            o.n_points = t.n_points;
            o.time_since_update_s = t.time_since_update_s;
            out.push_back(o);
        }
        return out;
    }

    void reset() { tm_.reset(); }
    const TrackManager& manager() const { return tm_; }

private:
    TrackManager tm_;
    ClusterParams cluster_;
    int min_points_;
};
