// ElevSafe 상태 머신 (C) — 룰표 v2.1 구현
// - Passenger Phase: OUTSIDE / ENTERING / INSIDE / EXITING
// - Safety Status  : NORMAL / FALL / IMMOBILE
// - door_command   : NONE(=null) / OPEN / HOLD / CLOSE
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
  bool  measured = true;      // 실제 측정된 프레임이면 true (LOST 예측 프레임이면 false)
};

struct FrameInput {
  double t = 0;               // 초 (timestamp_us / 1e6)
  DoorState door = DoorState::UNKNOWN;
  std::vector<PersonObs> persons;
};

struct Config {
  // --- FALL (변화량 + 하강 속도, 절대 높이 아님) ---
  double fall_window_sec     = 0.9;   // 무릎 꿇고 엎드리는 2단계 전도 포착
  double fall_drop_m         = 0.72;
  double fall_speed_mps      = 1.05;
  double baseline_band_m     = 0.15;  // 평소 높이 갱신 허용 범위(쪼그려 앉기가 기준을 끌어내리지 않게)
  double baseline_alpha      = 0.05;
  double warmup_sec          = 2.0;   // 트랙 생성 후 FALL 판정 보류, baseline은 이 구간 중앙값
  double baseline_down_alpha = 0.1;   // z가 안정적일 때 baseline을 아래로 당기는 비율
  double stable_range_m      = 0.15;  // 1초 창 z 변동폭이 이 이하면 "안정"
  bool   door_fall_enabled   = true;  // 출입구(DOOR) 구역 낙상 판정 활성화
  int    door_fall_min_points = 25;   // DOOR 구역 통과 중 포인트 손실에 의한 FALL 오탐 방지용 최소 점 수
  // --- 낮은 자세 / 회복 ---
  double low_drop_m          = 0.50;  // baseline - low_drop 이하면 낮은 자세
  double stand_margin_m      = 0.25;  // baseline - margin 이상이면 서 있음
  double recover_hold_sec    = 5.0;   // 회복 유지 시간(초기값)
  double immobile_sec        = 40.0;  // IMMOBILE 기준(초기값, 출입구·내부 공통)
  bool   use_doppler_gate    = true;  // 낮은 자세여도 움직임이 크면 IMMOBILE 타이머 리셋
  double move_doppler_thr    = 0.15;  // (m/s)
  // --- 추적 끊김 ---
  double lost_grace_sec      = 2.0;   // 1~2초 끊김은 사람이 있다고 간주
  double lost_grace_danger_sec = 5.0; // FALL/IMMOBILE 승객은 더 오래 상태 유지
  // --- Phase 방향 판정 ---
  double dir_window_sec      = 1.0;
  double dir_dy_m            = 0.10;
  bool   cabin_is_plus_y     = true;  // Y 캐빈 안쪽 +, 승강장 쪽 − (원점=문턱 띠 가운데)
  // --- 문 제어 ---
  bool   hall_enabled        = false; // false면 승강장 zone은 UNKNOWN 취급
  bool   hall_safety_enabled = false; // false면 HALL zone 승객은 FALL/IMMOBILE 판정 안 함
  double hold_max_sec        = 30.0;  // 초과 시 "경고"만, 문턱 점유 중 HOLD는 해제하지 않음
  double close_wait_sec      = 3.0;   // 통과 후 CLOSE까지 대기(설정값)
  bool   emit_close          = true;  // A가 정상 CLOSE를 담당하면 false
  double door_open_max_sec   = 60.0;  // 시설 측 문 열림 최대 시간
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
      // 미확정 DOOR 트랙(id < 0)은 즉시 사라지면 바로 정리, 확정 트랙은 grace 적용
      const double grace = (it->first < 0)
                               ? 0.25
                               : ((it->second.safety != Safety::NORMAL) ? cfg_.lost_grace_danger_sec
                                                                        : cfg_.lost_grace_sec);
      it = (t - it->second.last_seen > grace) ? tracks_.erase(it) : std::next(it);
    }

    Output out;
    bool occupied = false, door_danger = false;
    for (const auto& kv : tracks_) {
      const Track& k = kv.second;
      if (kv.first < 0) {
        // DOOR 구역 미확정(TENTATIVE) 트랙은 문턱 점유(HOLD) 판단에만 즉시 반영
        if (k.phase == Phase::ENTERING || k.phase == Phase::EXITING) occupied = true;
        continue;
      }
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
    double pending_fall_t = -1;          // 급하강 감지 후 실제 바닥 유지 검증 시작 시각
    double baseline = -1;
    double first_t = -1;                 // 트랙이 처음 측정된 시각
    std::vector<double> warm;            // 워밍업 구간 z 샘플
    std::deque<std::pair<double, double>> zhist, yhist;
    bool seen = false;
  };

  static void resetSafety(Track& k) {
    k.safety = Safety::NORMAL;
    k.fall_t = k.immobile_t = k.low_since = k.stand_since = k.pending_fall_t = -1;
    k.first_t = -1; k.baseline = -1;
    k.warm.clear(); k.zhist.clear();
  }

  void updateTrack(Track& k, const PersonObs& p, double t) {
    k.last_seen = t; k.zone = p.zone;
    if (!p.measured) return;             // 예측(LOST) 프레임은 z/y 이력에 넣지 않음
    if (k.first_t < 0) k.first_t = t;
    push(k.zhist, t, p.z_top, cfg_.fall_window_sec);
    push(k.yhist, t, p.y, cfg_.dir_window_sec);
    if (k.baseline < 0) k.baseline = p.z_top;
    updatePhase(k, p);
    if (p.id < 0) {                      // 미확정 DOOR 트랙은 Phase 및 문턱 점유만 업데이트
      k.seen = true;
      return;
    }
    if (p.zone == Zone::HALL && !cfg_.hall_safety_enabled) {
      resetSafety(k);
      return;
    }
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
      case Zone::UNKNOWN: break;
      case Zone::DOOR: {
        double dy = k.yhist.back().second - k.yhist.front().second;
        if (!cfg_.cabin_is_plus_y) dy = -dy;
        if (dy >= cfg_.dir_dy_m) k.phase = Phase::ENTERING;
        else if (dy <= -cfg_.dir_dy_m) k.phase = Phase::EXITING;
        else if (k.phase == Phase::INSIDE) k.phase = Phase::EXITING;
        else if (k.phase == Phase::OUTSIDE) k.phase = Phase::ENTERING;
        break;
      }
    }
  }

  void updateSafety(Track& k, const PersonObs& p, double t) {
    const double z = p.z_top;

    // 워밍업: 트랙 생성 직후에는 FALL 판정을 하지 않고 baseline을 z 중앙값으로 잡는다.
    if (t - k.first_t < cfg_.warmup_sec) {
      k.warm.push_back(z);
      auto w = k.warm;
      std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
      k.baseline = w[w.size() / 2];
      return;
    }

    // DOOR 구역에서는 활성화 플래그 및 최소 포인트 수 충족 시에만 신규 FALL 평가
    if (p.zone == Zone::DOOR && k.safety == Safety::NORMAL) {
      if (!cfg_.door_fall_enabled || p.num_points < cfg_.door_fall_min_points) {
        return;
      }
    }

    const bool standing = z >= k.baseline - cfg_.stand_margin_m;
    const bool low = z <= k.baseline - cfg_.low_drop_m;

    if (k.safety == Safety::NORMAL) {
      if (z > k.baseline - cfg_.baseline_band_m)
        k.baseline += cfg_.baseline_alpha * (z - k.baseline);
      if (z > k.baseline) k.baseline = z;
      {
        auto mm = std::minmax_element(k.zhist.begin(), k.zhist.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        if (!k.zhist.empty() && mm.second->second - mm.first->second < cfg_.stable_range_m
            && z < k.baseline)
          k.baseline += cfg_.baseline_down_alpha * (z - k.baseline);
      }
      if (fallCondition(k, z, t)) {
        if (k.pending_fall_t < 0) k.pending_fall_t = t;
      }
      if (k.pending_fall_t >= 0) {
        if (low) {
          if (t - k.pending_fall_t >= 0.8) {   // 0.8초 이상 실제 낮은 자세 유지 시 FALL 확정
            k.safety = Safety::FALL; k.fall_t = k.pending_fall_t; k.immobile_t = -1;
            k.low_since = t; k.stand_since = -1; k.pending_fall_t = -1;
          }
        } else {
          k.pending_fall_t = -1;
        }
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
    }
  }

  bool fallCondition(const Track& k, double z, double t) const {
    if (k.zhist.size() < 2) return false;
    auto peak = *std::max_element(k.zhist.begin(), k.zhist.end(),
        [](auto& a, auto& b) { return a.second < b.second; });
    const double drop = peak.second - z;
    const double dt = std::max(t - peak.first, 0.05);
    return drop >= cfg_.fall_drop_m && drop / dt >= cfg_.fall_speed_mps &&
           z <= k.baseline - cfg_.fall_drop_m;
  }

  void doorLogic(DoorState door, bool occupied, double t, Output& out) {
    if (door == DoorState::UNKNOWN) {
      closing_ = false; out.reason = "door UNKNOWN -> null"; return;
    }
    if (door == DoorState::CLOSED) {
      hold_since_ = clear_since_ = -1; closing_ = false; out.reason = "closed"; return;
    }
    // door == OPEN
    if (occupied) {
      closing_ = false;
      clear_since_ = -1;
      if (hold_since_ < 0) hold_since_ = t;
      out.door_command = DoorCommand::HOLD;
      out.hold_exceeded = (t - hold_since_ > cfg_.hold_max_sec);
      out.reason = "threshold occupied -> HOLD";
      return;
    }
    if (hold_since_ >= 0) {
      if (clear_since_ < 0) clear_since_ = t;
      if (cfg_.emit_close && t - clear_since_ >= cfg_.close_wait_sec) {
        out.door_command = DoorCommand::CLOSE; hold_since_ = clear_since_ = -1;
        closing_ = true;
        out.reason = "cleared -> CLOSE";
      } else {
        out.door_command = DoorCommand::HOLD; out.reason = "cleared, waiting";
      }
    } else if (closing_) {
      out.door_command = DoorCommand::CLOSE; out.reason = "CLOSE latched until door CLOSED";
    } else out.reason = "normal operation";
  }

  Config cfg_;
  std::map<int, Track> tracks_;
  double open_since_ = -1, hold_since_ = -1, clear_since_ = -1;
  bool closing_ = false;
};

}  // namespace elevsafe