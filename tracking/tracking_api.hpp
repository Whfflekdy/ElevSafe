// tracking/tracking_api.hpp
// 백엔드(A)가 함수 호출로 쓰는 추적 + 상태 판단 모듈의 입구.
//
// [실시간 권장] processFrame(fd, door_state)
//   입력: B가 만든 덩어리(FrameDetections) + 이 프레임의 문 상태
//   출력: 트랙별 (공개 ID, 위치, Phase, Safety) + 전체 door_command / safety_state / 경고
//   점이 하나도 없는 프레임도 반드시 호출할 것 (시간 흐름이 트랙 생명주기·끊김 처리에 필요).
//
// [기존 호환] processDetections(fd) / processPoints(...)
//   트랙 위치·속도만 반환 (Phase/Safety 판단 없음). 시그니처는 그대로.
//   같은 엔진에서 processFrame 과 섞어 쓰지 말 것 (한 프레임에 한 번만 호출).
//
// hall_enabled: C 설정값(elevsafe::Config.hall_enabled, 기본 false).
//   false 이면 zone == HALL 인 Detection 을 매칭·게이팅 전에 버린다 (승강장 사람이 트랙을 만들지 않게).
//   켜는 날은 A·B·C가 같이 바꾼다 (인터페이스 v4 9장).
//
// 사용 예:
//   TrackingEngine engine;                                   // 기본 설정
//   FrameDetections fd = ...;                                // A 어댑터: Cluster -> Detection (zone, z_top 포함!)
//   FrameResult r = engine.processFrame(fd, elevsafe::DoorState::OPEN);
//   // r.door_command (null/OPEN/HOLD/CLOSE), r.safety_state, r.tracks[i].phase / .safety
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "id_manager/id_manager.hpp"
#include "detection/detection_loader.hpp"
#include "elevsafe_types.hpp"
#include "state_machine.hpp"
#include "track_to_state.hpp"

struct TrackOutput {
    int         id = -1;                          // 공개 ID (CONFIRMED가 되면 부여, 그 전에는 -1)
    TrackState  state = TrackState::TENTATIVE;
    double      x = 0, y = 0, z = 0;              // 위치 [m]
    double      vx = 0, vy = 0, vz = 0;           // 속도 [m/s]
    int         n_points = 0;                     // 마지막 매칭 검출의 점 개수
    double      time_since_update_s = 0;          // 마지막으로 검출과 매칭된 뒤 경과 시간 [s]
    // --- 아래는 processFrame() 에서만 채워진다 ---
    elevsafe::DetZone zone = elevsafe::DetZone::UNKNOWN;   // 마지막 매칭 검출의 zone
    double      z_top = 0;                        // 마지막 매칭 검출의 z_top [m]
    bool        lost = false;                     // 현재 LOST (예측 위치)
    elevsafe::Phase  phase  = elevsafe::Phase::INSIDE;     // 위치 단계
    elevsafe::Safety safety = elevsafe::Safety::NORMAL;    // 안전 상태
};

// processFrame() 결과 (프레임당 1개)
struct FrameResult {
    int64_t frame = 0;
    int64_t timestamp_us = 0;
    std::vector<TrackOutput> tracks;               // CONFIRMED + LOST 트랙 (TENTATIVE 제외)
    elevsafe::DoorCommand door_command = elevsafe::DoorCommand::NONE;  // NONE = JSON null
    elevsafe::Safety safety_state = elevsafe::Safety::NORMAL;          // 전체 중 가장 위험한 값 (A–D JSON safety_state)
    bool        hold_exceeded = false;             // HOLD 가 hold_max_sec 초과 (경고만, HOLD 는 해제하지 않음)
    bool        door_open_limit_warning = false;   // 출입구 낙상 + 문 열림 한계 임박
    std::string reason;                            // 디버그/로그용
};

class TrackingEngine {
public:
    explicit TrackingEngine(const TrackerConfig& cfg = TrackerConfig(),
                            const ClusterParams& cluster = ClusterParams(),
                            int min_points = 3,
                            const elevsafe::Config& state_cfg = elevsafe::Config())
        : tm_(cfg), cluster_(cluster), min_points_(min_points), sm_(state_cfg) {}

    // 원시 점으로 호출 (점을 직접 묶는다). 실시간 경로에서는 쓰지 않는다 (B의 처리 결과를 버리게 됨).
    std::vector<TrackOutput> processPoints(int64_t frame, int64_t timestamp_us,
                                           const std::vector<PointIn>& points,
                                           bool confirmed_only = true) {
        FrameDetections fd;
        fd.frame = frame;
        fd.timestamp_us = timestamp_us;
        fd.detections = detectionsFromPoints(points, min_points_, cluster_);
        return processDetections(fd, confirmed_only);
    }

    // 이미 묶은 검출로 호출. 트랙 위치/속도만 반환 (기존 동작).
    std::vector<TrackOutput> processDetections(const FrameDetections& fd, bool confirmed_only = true) {
        const std::vector<Track>& tracks = runTracking(fd);
        std::vector<TrackOutput> out;
        for (const Track& t : tracks) {
            if (confirmed_only && t.state != TrackState::CONFIRMED) continue;
            out.push_back(makeOutput(t));
        }
        return out;
    }

    // 추적 + 상태 판단 + 문 제어 요청. door 를 모르면 UNKNOWN (door_command 는 null).
    FrameResult processFrame(const FrameDetections& fd,
                             elevsafe::DoorState door = elevsafe::DoorState::UNKNOWN) {
        const std::vector<Track>& tracks = runTracking(fd);

        elevsafe::FrameInput in;
        in.t = fd.timestamp_us * 1e-6;
        in.door = door;
        in.persons = elevsafe::toPersonObs(tracks);
        const elevsafe::Output st = sm_.update(in);

        FrameResult r;
        r.frame = fd.frame;
        r.timestamp_us = fd.timestamp_us;
        r.door_command = st.door_command;
        r.safety_state = st.safety_state;
        r.hold_exceeded = st.hold_exceeded;
        r.door_open_limit_warning = st.door_open_limit_warning;
        r.reason = st.reason;
        for (const Track& t : tracks) {
            if (t.state == TrackState::TENTATIVE || t.id < 0) continue;   // 공개 ID 없는 트랙 제외
            TrackOutput o = makeOutput(t);
            for (const elevsafe::TrackStatus& s : st.tracks)
                if (s.id == t.id) { o.phase = s.phase; o.safety = s.safety; break; }
            r.tracks.push_back(o);
        }
        return r;
    }

    // 세션 초기화·재연결: 트랙, ID 번호, 상태 머신(낙상/부동/HOLD/CLOSE 상태)을 모두 지운다.
    void reset() { tm_.reset(); sm_.reset(); }

    // HALL 처리 켜기/끄기 (A의 SafetyConfig.hall_enabled 와 맞출 것)
    void setHallEnabled(bool v) { sm_.setHallEnabled(v); }
    bool hallEnabled() const { return sm_.config().hall_enabled; }

    const TrackManager& manager() const { return tm_; }

private:
    // hall_enabled 가 꺼져 있으면 zone == HALL 검출을 버린 뒤 추적한다.
    const std::vector<Track>& runTracking(const FrameDetections& fd) {
        if (!sm_.config().hall_enabled) {
            bool has_hall = false;
            for (const Detection& d : fd.detections)
                if (d.zone == elevsafe::DetZone::HALL) { has_hall = true; break; }
            if (has_hall) {
                FrameDetections f;
                f.frame = fd.frame;
                f.timestamp_us = fd.timestamp_us;
                for (const Detection& d : fd.detections)
                    if (d.zone != elevsafe::DetZone::HALL) f.detections.push_back(d);
                return tm_.update(f);
            }
        }
        return tm_.update(fd);
    }

    static TrackOutput makeOutput(const Track& t) {
        TrackOutput o;
        o.id = t.id;
        o.state = t.state;
        o.x = t.x(); o.y = t.y(); o.z = t.z();
        o.vx = t.vx(); o.vy = t.vy(); o.vz = t.vz();
        o.n_points = t.n_points;
        o.time_since_update_s = t.time_since_update_s;
        o.zone = t.det_zone;
        o.z_top = t.det_z_top;
        o.lost = (t.state == TrackState::LOST);
        return o;
    }

    TrackManager tm_;
    ClusterParams cluster_;
    int min_points_;
    elevsafe::StateMachine sm_;
};
