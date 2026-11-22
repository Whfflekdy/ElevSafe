// track_to_state.hpp -- TrackManager 출력(Track) -> StateMachine 입력(PersonObs) 어댑터 (header-only)
// 예전 track_to_safety.hpp(SafetyJudge용)를 대체한다.
#pragma once
#include <vector>
#include "elevsafe_types.hpp"
#include "id_manager/id_manager.hpp"
#include "state_machine.hpp"

namespace elevsafe {

// DetZone(B 숫자 체계) -> 상태 머신의 Zone
inline Zone toStateZone(DetZone z) {
    switch (z) {
        case DetZone::DOOR:  return Zone::DOOR;
        case DetZone::CABIN: return Zone::CABIN;
        case DetZone::HALL:  return Zone::HALL;
        default:             return Zone::UNKNOWN;
    }
}

// 이번 프레임에 검출과 매칭된 CONFIRMED 트랙만 넘긴다.
//  - TENTATIVE: 공개 ID가 없으므로 제외
//  - LOST(이번 프레임 매칭 없음): 넘기지 않는다. 끊김 처리(lost_grace_sec / lost_grace_danger_sec)는
//    상태 머신이 마지막으로 본 시각 기준으로 한다.
inline std::vector<PersonObs> toPersonObs(const std::vector<Track>& tracks) {
    std::vector<PersonObs> out;
    out.reserve(tracks.size());
    for (const Track& t : tracks) {
        if (t.state != TrackState::CONFIRMED || t.id < 0) continue;
        if (t.time_since_update_s > 1e-9) continue;       // 이번 프레임에 매칭되지 않음
        PersonObs p;
        p.id = t.id;
        p.zone = toStateZone(t.det_zone);
        p.x = static_cast<float>(t.x());
        p.y = static_cast<float>(t.y());
        p.z_top = static_cast<float>(t.det_z_top);
        p.z_min = static_cast<float>(t.det_z_min);
        p.cz = static_cast<float>(t.det_cz);
        p.num_points = t.n_points;
        p.mean_doppler = static_cast<float>(t.det_mean_doppler);
        out.push_back(p);
    }
    return out;
}

inline const char* toString(Phase p) {
    switch (p) {
        case Phase::OUTSIDE:  return "OUTSIDE";
        case Phase::ENTERING: return "ENTERING";
        case Phase::INSIDE:   return "INSIDE";
        case Phase::EXITING:  return "EXITING";
    }
    return "?";
}
inline const char* toString(Safety s) {
    switch (s) {
        case Safety::NORMAL:    return "NORMAL";
        case Safety::FALL:      return "FALL";
        case Safety::IMMOBILE:  return "IMMOBILE";
    }
    return "?";
}
inline const char* toString(DoorCommand c) {
    switch (c) {
        case DoorCommand::NONE:  return "null";     // A-D JSON 에서 door_command = null
        case DoorCommand::OPEN:  return "OPEN";
        case DoorCommand::HOLD:  return "HOLD";
        case DoorCommand::CLOSE: return "CLOSE";
    }
    return "?";
}

}  // namespace elevsafe
