// door_state_estimator.cpp
#include "preprocessing/door_state_estimator.h"

namespace elevsafe::sp {

const char* doorStateToString(DoorState s) {
    switch(s){
        case DoorState::OPEN:    return "OPEN";
        case DoorState::CLOSED:  return "CLOSED";
        case DoorState::UNKNOWN: break;
    }
    return "UNKNOWN";
}

DoorStateConfig DoorStateConfig::fromConfig(const Config& cfg) {
    DoorStateConfig c;
    // TODO: door_state.view.x_min ~ z_max 여섯 값 읽기
    //       기본값: x -0.44 ~ 0.44, y -3.00 ~ -0.30, z 0.10 ~ 2.10
    //       (roi_filter.cpp 의 readBox 와 같은 방식. 그 함수는 헤더에 없어서 여기선 직접 6번 getFloat)
    c.view_zone.x_min = cfg.getFloat("door_state.view.x_min", -0.44f);
    c.view_zone.x_max = cfg.getFloat("door_state.view.x_max",  0.44f);
    c.view_zone.y_min = cfg.getFloat("door_state.view.y_min", -3.00f);
    c.view_zone.y_max = cfg.getFloat("door_state.view.y_max", -0.30f);
    c.view_zone.z_min = cfg.getFloat("door_state.view.z_min",  0.10f);
    c.view_zone.z_max = cfg.getFloat("door_state.view.z_max",  2.10f);

    // door_state.window 읽기 (getFloat 로 읽고 std::size_t 로 변환, 기본 10)
    const float w = cfg.getFloat("door_state.window", 10.0f);
    c.window = (w < 1.0f)? 1: static_cast<std::size_t>(w);
    // door_state.open_threshold (기본 10), door_state.close_threshold (기본 1)
    c.open_threshold = cfg.getFloat("door_state.open_threshold", 10.0f);
    c.close_threshold = cfg.getFloat("door_state.close_threshold", 1.0f);
    return c;
}

DoorStateEstimator::DoorStateEstimator(const DoorStateConfig& config) : config_(config) {}

DoorState DoorStateEstimator::update(const Frame& points) {
    // 1. 이번 프레임에서 view_zone(문 너머 상자 안)에 든 점 개수 세기
    std::size_t n =0;
    for(const auto &p: points){
        if(config_.view_zone.contains(p)) ++n;
    }
    last_count_ = n;

    // 2. 최근 window 프레임만 기억(뒤에 넣고 넘치면 앞에서 뺌)
    counts_.push_back(n);
    if(counts_.size() > config_.window) counts_.pop_front();

    // 3. 평균 계산
    std::size_t sum = 0;
    for(std::size_t c: counts_) sum+=c;
    average_ = static_cast<float>(sum) / static_cast<float>(counts_.size());

    // 4. 아직 window 만큼 안 쌓였으면 판단 보류(처음 약 0.5초는 UNKNOWN)
    if(counts_.size() < config_.window) return state_;

    // 5. 히스테리시스로 상태 결정
    // state_ 가 OPEN 이 아니고 average_ >= open_threshold  → OPEN
    if(state_ != DoorState::OPEN && average_>=config_.open_threshold){
        state_= DoorState::OPEN;
    // state_ 가 CLOSED 가 아니고 average_ <= close_threshold → CLOSED
    // 둘 다 아니면 그대로 유지
    // (처음 UNKNOWN 에서는 둘 중 맞는 쪽으로, 중간 값이면 UNKNOWN 유지)
    }else if(state_ != DoorState::CLOSED && average_ <= config_.close_threshold){
        state_ = DoorState::CLOSED;
    }

    return state_;
}

}  // namespace elevsafe::sp
