// door_state_estimator.h
// 문 상태 추정: 문 너머(승강장 쪽) 상자 안에 점이 보이면 열림, 안 보이면 닫힘.
// 닫힌 금속 문은 전파를 막고 자기는 잘 안 찍히므로, "문이 보이나"가 아니라 "문 너머가 보이나"로 판단한다.
// 좌표 변환 직후, ROI 전에 호출해야 한다 (ROI가 문 너머 점을 지우기 전).
#pragma once

#include <deque>
#include <cstddef>

#include "common/radar_point.h"
#include "common/box3d.h"
#include "common/config.h"

// 나중에 SignalPipeline 헤더(elevsafe::sp)로 옮길 예정
enum class DoorState { UNKNOWN, OPEN, CLOSED };
const char* doorStateToString(DoorState s);

struct DoorStateConfig {
    Box3D view_zone;              // 문 너머를 보는 상자 (공통 좌표)
    std::size_t window = 10;      // 최근 몇 프레임을 평균낼지
    float open_threshold = 10.0f; // 평균이 이 이상이면 OPEN
    float close_threshold = 1.0f; // 평균이 이 이하면 CLOSED

    static DoorStateConfig fromConfig(const Config& cfg);
    bool isValid() const {
        return view_zone.isValid() && window > 0 && close_threshold < open_threshold;
    }
};

class DoorStateEstimator {
public:
    explicit DoorStateEstimator(const DoorStateConfig& config);

    // 한 프레임(좌표 변환 후 전체 점)을 넣으면 현재 문 상태를 돌려준다
    DoorState update(const Frame& points);

    DoorState state() const { return state_; }
    std::size_t lastCount() const { return last_count_; } // 이번 프레임의 문 너머 점 수
    float average() const { return average_; }            // 최근 window 프레임 평균

private:
    DoorStateConfig config_;
    std::deque<std::size_t> counts_;  // 최근 window 프레임의 점 수
    DoorState state_ = DoorState::UNKNOWN;
    std::size_t last_count_ = 0;
    float average_ = 0.0f;
};