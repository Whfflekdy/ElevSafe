// ElevSafe 상태 머신 (C) — 룰표 v2(2026-11-19) 구현
// - Passenger Phase: OUTSIDE / ENTERING / INSIDE / EXITING
// - Safety Status  : NORMAL / FALL / IMMOBILE
// - door_command   : NONE(=null) / OPEN / HOLD / CLOSE
// 모든 임계값은 Config로 분리. "TUNE" 표시 값은 4차 녹화 데이터로 정할 임시값(실측값 아님).
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "elevsafe_types.hpp"   // DoorState (cluster_loader.hpp 와 공유)

namespace elevsafe {

enum class Zone { UNKNOWN, CABIN, DOOR, HALL };               // B 판정 → A 전달
enum class Phase { OUTSIDE, ENTERING, INSIDE, EXITING };
enum class Safety { NORMAL, FALL, IMMOBILE };
enum class DoorCommand { NONE /*null*/, OPEN, HOLD, CLOSE };

struct PersonObs {            // 프레임당 추적 객체 1명
  int   id = -1;
  Zone  zone = Zone::UNKNOWN;
  float x = 0, y = 0;
  float z_top = 0, z_min = 0, cz = 0;
  int   num_points = 0;
  float mean_doppler = 0;
};

struct FrameInput {
  double t = 0;               // 초 (timestamp_us / 1e6)
  DoorState door = DoorState::UNKNOWN;
  std::vector<PersonObs> persons;
};

struct Config {
  // --- FALL (TUNE: 변화량 + 하강 속도, 절대 높이 아님) ---
  double fall_window_sec   = 1.0;   // 이 창 안의 최고 z_top 대비 하강량으로 판단
  double fall_drop_m       = 0.60;  // TUNE
  double fall_speed_mps    = 0.80;  // TUNE
  double baseline_band_m   = 0.15;  // 평소 높이 갱신 허용 범위(쪼그려 앉기가 기준을 끌어내리지 않게)
  double baseline_alpha    = 0.05;
  // --- 낮은 자세 / 회복 ---
  double low_drop_m        = 0.50;  // TUNE: baseline - low_drop 이하면 낮은 자세
  double stand_margin_m    = 0.25;  // TUNE: baseline - margin 이상이면 서 있음
  double recover_hold_sec  = 5.0;   // 회복 유지 시간(초기값)
  double immobile_sec      = 40.0;  // IMMOBILE 기준(초기값, 출입구·내부 공통)
  bool   use_doppler_gate  = true;  // 낮은 자세여도 움직임이 크면 IMMOBILE 타이머 리셋
  double move_doppler_thr  = 0.15;  // TUNE (m/s)
  // --- 추적 끊김 ---
  double lost_grace_sec    = 2.0;   // 1~2초 끊김은 사람이 있다고 간주
  double lost_grace_danger_sec = 5.0; // FALL/IMMOBILE 승객은 더 오래 상태 유지 (TrackManager max_age_s 기본값과 같게)
  // --- Phase 방향 판정 ---
  double dir_window_sec    = 1.0;
  double dir_dy_m          = 0.10;
  bool   cabin_is_plus_y   = true;  // 인터페이스 v4: Y 캐빈 안쪽 +, 승강장 쪽 − (원점=문턱 띠 가운데)
  // --- 문 제어 ---
  bool   hall_enabled      = false; // false면 승강장 zone은 UNKNOWN 취급
  double hold_max_sec      = 30.0;  // 초과 시 "경고"만, 문턱 점유 중 HOLD는 해제하지 않음
  double close_wait_sec    = 3.0;   // 통과 후 CLOSE까지 대기(설정값)
  bool   emit_close        = true;  // A가 정상 CLOSE를 담당하면 false
  double door_open_max_sec = 60.0;  // 시설 측 문 열림 최대 시간
  double door_open_warn_lead_sec = 10.0;
};

struct TrackStatus {
  int id; Phase phase; Safety safety;
  double fall_detect_t;   // FALL 조건 최초 충족 시각(감지 시점). 없으면 -1
  double immobile_t;      // IMMOBILE 확정 시각. 없으면 -1
};

struct Output {
  DoorCommand door_command = DoorCommand::NONE;
  Safety safety_state = Safety::NORMAL;      // 전체 중 가장 위험한 값 → A–D JSON v7 safety_state
  std::vector<TrackStatus> tracks;
  bool hold_exceeded = false;                // HOLD가 hold_max_sec 초과 → 관리자 대응
  bool door_open_limit_warning = false;      // 출입구 낙상 + 문 열림 1분 임박 → 관제 경보
  std::string reason;                        // 디버그용
};

class StateMachine {
 public:
  explicit StateMachine(Config cfg = {}) : cfg_(cfg) {}

  // 새 녹화/세션 시작, 재연결 시 호출: 모든 트랙 상태와 문 제어 상태를 지운다.
  void reset() {
    tracks_.clear();
    open_since_ = hold_since_ = clear_since_ = -1;
    closing_ = false;
  }
  const Config& config() const { return cfg_; }
  void setHallEnabled(bool v) { cfg_.hall_enabled = v; }

  Output update(const FrameInput& in) {
    const double t = in.t;
    for (const auto& p : in.persons) updateTrack(tracks_[p.id], p, t);
    for (auto it = tracks_.begin(); it != tracks_.end();) {
      // FALL/IMMOBILE 승객은 추적이 끊겨도 lost_grace_danger_sec 동안 상태를 유지한다.
      const double grace = (it->second.safety != Safety::NORMAL) ? cfg_.lost_grace_danger_sec
                                                                  : cfg_.lost_grace_sec;
      it = (t - it->second.last_seen > grace) ? tracks_.erase(it) : std::next(it);
    }

    Output out;
    bool occupied = false, door_danger = false;
    for (const auto& kv : tracks_) {
      const Track& k = kv.second;
      out.tracks.push_back({kv.first, k.phase, k.safety, k.fall_t, k.immobile_t});
      if (static_cast<int>(k.safety) > static_cast<int>(out.safety_state)) out.safety_state = k.safety;
      if (k.phase == Phase::ENTERING || k.phase == Phase::EXITING) {
        occupied = true;
        if (k.safety != Safety::NORMAL) door_danger = true;
      }
    }

    // 문 열림 시간 추적
    if (in.door == DoorState::OPEN) { if (open_since_ < 0) open_since_ = t; }
    else open_since_ = -1;

    doorLogic(in.door, occupied, t, out);

    if (door_danger && open_since_ >= 0 &&
        t - open_since_ >= cfg_.door_open_max_sec - cfg_.door_open_warn_lead_sec)
      out.door_open_limit_warning = true;
    return out;
  }

 private:
  struct Track {
    Phase phase = Phase::INSIDE; Zone zone = Zone::UNKNOWN; Safety safety = Safety::NORMAL;
    double last_seen = 0, fall_t = -1, immobile_t = -1, low_since = -1, stand_since = -1;
    double baseline = -1;
    std::deque<std::pair<double, double>> zhist, yhist;
    bool seen = false;
  };

  void updateTrack(Track& k, const PersonObs& p, double t) {
    k.last_seen = t; k.zone = p.zone;
    push(k.zhist, t, p.z_top, cfg_.fall_window_sec);
    push(k.yhist, t, p.y, cfg_.dir_window_sec);
    if (k.baseline < 0) k.baseline = p.z_top;
    updatePhase(k, p);
    updateSafety(k, p, t);
    k.seen = true;
  }

  static void push(std::deque<std::pair<double, double>>& h, double t, double v, double win) {
    h.emplace_back(t, v);
    while (!h.empty() && t - h.front().first > win) h.pop_front();
  }

  void updatePhase(Track& k, const PersonObs& p) {
    switch (p.zone) {
      case Zone::CABIN: k.phase = Phase::INSIDE; break;
      case Zone::HALL:  k.phase = cfg_.hall_enabled ? Phase::OUTSIDE : k.phase; break;
      case Zone::UNKNOWN: break;                       // 정보 없음 → 이전 Phase 유지
      case Zone::DOOR: {
        double dy = k.yhist.back().second - k.yhist.front().second;
        if (!cfg_.cabin_is_plus_y) dy = -dy;
        if (dy >= cfg_.dir_dy_m) k.phase = Phase::ENTERING;
        else if (dy <= -cfg_.dir_dy_m) k.phase = Phase::EXITING;
        else if (k.phase == Phase::INSIDE) k.phase = Phase::EXITING;   // 방향 불명확 시 직전 위치로 추정
        else if (k.phase == Phase::OUTSIDE) k.phase = Phase::ENTERING;
        break;  // 이미 ENTERING/EXITING이면 유지 (어느 쪽이든 닫힘 보류)
      }
    }
  }

  void updateSafety(Track& k, const PersonObs& p, double t) {
    const double z = p.z_top;
    const bool standing = z >= k.baseline - cfg_.stand_margin_m;
    const bool low = z <= k.baseline - cfg_.low_drop_m;

    if (k.safety == Safety::NORMAL) {
      if (z > k.baseline - cfg_.baseline_band_m)       // 서 있는 높이 근처에서만 기준 갱신
        k.baseline += cfg_.baseline_alpha * (z - k.baseline);
      if (z > k.baseline) k.baseline = z;
      if (fallCondition(k, z, t)) {
        k.safety = Safety::FALL; k.fall_t = t; k.immobile_t = -1;
        k.low_since = t; k.stand_since = -1;
      }
      return;
    }

    // FALL / IMMOBILE: 회복 판단
    if (standing) {
      if (k.stand_since < 0) k.stand_since = t;
      if (t - k.stand_since >= cfg_.recover_hold_sec) {
        k.safety = Safety::NORMAL; k.fall_t = k.immobile_t = k.low_since = k.stand_since = -1;
      }
      return;
    }
    k.stand_since = -1;
    if (k.safety == Safety::FALL) {
      if (low) {
        if (cfg_.use_doppler_gate && std::fabs(p.mean_doppler) > cfg_.move_doppler_thr) k.low_since = t;
        if (k.low_since < 0) k.low_since = t;
        if (t - k.low_since >= cfg_.immobile_sec) { k.safety = Safety::IMMOBILE; k.immobile_t = t; }
      }
      // 낮지도 서 있지도 않은(앉은/기는) 구간은 타이머를 그대로 둠
    }
  }

  bool fallCondition(const Track& k, double z, double t) const {
    if (k.zhist.size() < 2) return false;
    auto peak = *std::max_element(k.zhist.begin(), k.zhist.end(),
        [](auto& a, auto& b) { return a.second < b.second; });
    const double drop = peak.second - z;
    const double dt = std::max(t - peak.first, 0.05);
    return drop >= cfg_.fall_drop_m && drop / dt >= cfg_.fall_speed_mps &&
           z <= k.baseline - cfg_.fall_drop_m * 0.5;
  }

  void doorLogic(DoorState door, bool occupied, double t, Output& out) {
    if (door == DoorState::UNKNOWN) {                              // 룰: null 유지, 신규/유지 명령 모두 없음
      closing_ = false; out.reason = "door UNKNOWN -> null"; return;
    }
    if (door == DoorState::CLOSED) {                               // 닫힘 확인되면 CLOSE 유지 종료
      hold_since_ = clear_since_ = -1; closing_ = false; out.reason = "closed"; return;
    }
    // door == OPEN
    if (occupied) {
      closing_ = false;                                            // 닫는 중에 다시 점유되면 CLOSE 취소, HOLD로 복귀
      clear_since_ = -1;
      if (hold_since_ < 0) hold_since_ = t;
      out.door_command = DoorCommand::HOLD;
      out.hold_exceeded = (t - hold_since_ > cfg_.hold_max_sec);   // 경고만, 해제 안 함
      out.reason = "threshold occupied -> HOLD";
      return;
    }
    if (hold_since_ >= 0) {                                        // 우리가 HOLD하던 문
      if (clear_since_ < 0) clear_since_ = t;
      if (cfg_.emit_close && t - clear_since_ >= cfg_.close_wait_sec) {
        out.door_command = DoorCommand::CLOSE; hold_since_ = clear_since_ = -1;
        closing_ = true;                                           // 문이 CLOSED 로 바뀔 때까지 CLOSE 유지
        out.reason = "cleared -> CLOSE";
      } else {
        out.door_command = DoorCommand::HOLD; out.reason = "cleared, waiting";
      }
    } else if (closing_) {                                         // CLOSE 를 낸 뒤 아직 문이 열려 있음
      out.door_command = DoorCommand::CLOSE; out.reason = "CLOSE latched until door CLOSED";
    } else out.reason = "normal operation";                        // A의 정상 운행에 맡김
  }

  Config cfg_;
  std::map<int, Track> tracks_;
  double open_since_ = -1, hold_since_ = -1, clear_since_ = -1;
  bool closing_ = false;   // CLOSE 명령을 내고 문이 닫히기를 기다리는 중
};

}  // namespace elevsafe
