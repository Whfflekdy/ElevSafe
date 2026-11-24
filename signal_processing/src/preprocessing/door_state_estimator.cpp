// door_state_estimator.cpp
#include "preprocessing/door_state_estimator.h"

#include <cmath>

namespace elevsafe::sp {

const char* doorStateToString(DoorState s) {
    switch(s){
        case DoorState::OPEN:    return "OPEN";
        case DoorState::CLOSED:  return "CLOSED";
        case DoorState::OPENING: return "OPENING";
        case DoorState::CLOSING: return "CLOSING";
        case DoorState::UNKNOWN: break;
    }
    return "UNKNOWN";
}

DoorStateConfig DoorStateConfig::fromConfig(const Config& cfg) {
    DoorStateConfig c;
    // (1) 문 너머 상자 (원점: 문턱 띠 가운데)
    c.view_zone.x_min = cfg.getFloat("door_state.view.x_min", -0.44f);
    c.view_zone.x_max = cfg.getFloat("door_state.view.x_max",  0.44f);
    c.view_zone.y_min = cfg.getFloat("door_state.view.y_min", -2.88f);
    c.view_zone.y_max = cfg.getFloat("door_state.view.y_max", -0.18f);
    c.view_zone.z_min = cfg.getFloat("door_state.view.z_min",  0.10f);
    c.view_zone.z_max = cfg.getFloat("door_state.view.z_max",  2.10f);

    const float w = cfg.getFloat("door_state.window", 10.0f);
    c.window = (w < 1.0f) ? 1 : static_cast<std::size_t>(w);
    c.open_threshold  = cfg.getFloat("door_state.open_threshold", 10.0f);
    c.close_threshold = cfg.getFloat("door_state.close_threshold", 1.0f);

    // (2) 문짝 움직임. 상자는 1·2·4차 분석값 (문 폭보다 조금 넓게, 문턱 근처, 머리 위)
    c.motion_enabled = cfg.getFloat("door_state.motion_enable", 0.0f) != 0.0f;
    c.motion_zone.x_min = cfg.getFloat("door_state.motion.x_min", -0.60f);
    c.motion_zone.x_max = cfg.getFloat("door_state.motion.x_max",  0.60f);
    c.motion_zone.y_min = cfg.getFloat("door_state.motion.y_min", -0.18f);
    c.motion_zone.y_max = cfg.getFloat("door_state.motion.y_max",  0.27f);
    c.motion_zone.z_min = cfg.getFloat("door_state.motion.z_min",  1.80f);
    c.motion_zone.z_max = cfg.getFloat("door_state.motion.z_max",  2.40f);
    c.motion_min_doppler = cfg.getFloat("door_state.motion.min_doppler", 0.05f);
    c.motion_on  = cfg.getFloat("door_state.motion.on",  100.0f);
    c.motion_off = cfg.getFloat("door_state.motion.off",  30.0f);
    c.motion_reverse_speed = cfg.getFloat("door_state.motion.reverse_speed", 0.10f);
    return c;
}

DoorStateEstimator::DoorStateEstimator(const DoorStateConfig& config) : config_(config) {}

DoorState DoorStateEstimator::updateView(std::size_t n) {
    last_count_ = n;
    counts_.push_back(n);
    if (counts_.size() > config_.window) counts_.pop_front();

    std::size_t sum = 0;
    for (std::size_t c : counts_) sum += c;
    average_ = static_cast<float>(sum) / static_cast<float>(counts_.size());

    // window 만큼 쌓이기 전(처음 약 0.5초)은 판단 보류
    if (counts_.size() < config_.window) return view_state_;

    // 히스테리시스: 중간 값이면 이전 상태 유지
    if (view_state_ != DoorState::OPEN && average_ >= config_.open_threshold)
        view_state_ = DoorState::OPEN;
    else if (view_state_ != DoorState::CLOSED && average_ <= config_.close_threshold)
        view_state_ = DoorState::CLOSED;
    return view_state_;
}

void DoorStateEstimator::updateMotion(const Frame& points) {
    // 1. 이번 프레임에서 문짝 상자 안의 "움직이는 점" 수와 도플러 합
    std::size_t m = 0;
    float dsum = 0.0f;
    for (const auto& p : points) {
        if (config_.motion_zone.contains(p) && std::fabs(p.doppler) >= config_.motion_min_doppler) {
            ++m;
            dsum += p.doppler;
        }
    }

    // 2. 최근 window 프레임 합
    motion_.push_back({m, dsum});
    if (motion_.size() > config_.window) motion_.pop_front();
    std::size_t msum = 0;
    double wdsum = 0.0;
    for (const auto& [c, d] : motion_) { msum += c; wdsum += d; }
    motion_sum_ = msum;

    // 3. 움직임 시작: 합이 on 이상.
    //    움직이는 동안은 OPEN, 움직임이 끝나면 쌓인 도플러 합이 −면 CLOSED.
    if (!moving_) {
        if (static_cast<float>(msum) >= config_.motion_on) {
            moving_ = true;
            motion_seen_ = true;
            event_dsum_ = wdsum;                 // 시작한 window 의 도플러부터 쌓기
            state_ = (wdsum < 0.0) ? DoorState::CLOSING : DoorState::OPENING;   // 시작 window 도플러 부호로 방향
        }
        return;
    }

    // 4. 움직이는 중: 도플러를 계속 쌓다가, 합이 off 이하로 떨어지면 끝
    event_dsum_ += dsum;

    //    방향 바뀜: 강한 움직임이 평균 reverse_speed 이상으로 지금까지와 반대 방향이면 그때부터 새로 쌓음.
    //    닫히다가 장애물로 다시 열리는 경우 OPEN으로 끝나게 함 (A 리뷰, PR #45).
    //    닫힘 끝의 약한 + 꼬리(평균 0.07 이하)는 여기에 안 걸림 (1차 S0-3)
    if (static_cast<float>(msum) >= config_.motion_on) {
        const double mean = wdsum / static_cast<double>(msum);
        if (std::fabs(mean) >= config_.motion_reverse_speed && (mean > 0.0) != (event_dsum_ > 0.0))
            event_dsum_ = wdsum;
    }

    if (static_cast<float>(msum) <= config_.motion_off) {
        moving_ = false;
        state_ = (event_dsum_ < 0.0) ? DoorState::CLOSED : DoorState::OPEN;
    }
}

DoorState DoorStateEstimator::update(const Frame& points) {
    // (1) 문 너머 보기: 항상 계산 (꺼져 있을 때 결과, 켜져 있을 때 시작 상태)
    std::size_t n = 0;
    for (const auto& p : points)
        if (config_.view_zone.contains(p)) ++n;
    const DoorState view = updateView(n);

    if (!config_.motion_enabled) {
        state_ = view;                 // 기존 방식 그대로
        return state_;
    }

    // (2) 문짝 움직임
    updateMotion(points);
    // 문짝 움직임 방식에서는 문 너머 보기를 쓰지 않음. 첫 움직임 전까지는 UNKNOWN.
    //  4차: 승강장이 비면 열린 문을 CLOSED로(S0-2), 캐빈에 사람이 있으면 거울상 때문에
    //  닫힌 문을 OPEN으로(S1-8_t02, S1-5) 봐서, 시작 상태로도 믿을 수 없음.
    (void)view;
    return state_;
}

}  // namespace elevsafe::sp