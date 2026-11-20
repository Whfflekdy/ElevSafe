// door_state_estimator.h
// 문 상태 추정. 두 가지 방식:
//   (1) 문 너머 보기 (기본): 승강장 쪽 상자에 점이 보이면 열림, 안 보이면 닫힘.
//       승강장에 반사물이 없으면 열린 문을 닫힘으로 잘못 봄 (2차, 4차).
//   (2) 문짝 움직임 (door_state.motion_enable = 1): 문 위쪽 상자에서 움직이는 점이
//       많이 찍히면 문이 움직이는 중. 도플러 부호로 열림(+) / 닫힘(−)을 구분.
//       첫 움직임 전까지는 (1)로 시작 상태를 정하고, 그 뒤로는 움직임만 따른다.
// 좌표 변환 직후, ROI 전에 호출해야 한다 (ROI가 문 너머·문 위쪽 점을 지우기 전).
#pragma once

#include <deque>
#include <cstddef>
#include <utility>

#include "common/radar_point.h"
#include "common/box3d.h"
#include "common/config.h"
#include "common/door_state.h"

namespace elevsafe::sp {

struct DoorStateConfig {
    // --- (1) 문 너머 보기 ---
    Box3D view_zone;              // 문 너머를 보는 상자 (공통 좌표)
    std::size_t window = 10;      // 최근 몇 프레임을 볼지 (두 방식 공통, 약 0.5초)
    float open_threshold = 10.0f; // 평균이 이 이상이면 OPEN
    float close_threshold = 1.0f; // 평균이 이 이하면 CLOSED

    // --- (2) 대각선 쪽 문짝 움직임 ---
    bool motion_enabled = false;      // door_state.motion_enable. 꺼져 있으면 (1)만 씀 (기존 결과 그대로)
    Box3D motion_zone;                // 문짝이 지나가는 상자 (문 위쪽, 사람 머리 위)
    float motion_min_doppler = 0.05f; // 이보다 느린 점은 안 셈 [m/s]
    float motion_on = 100.0f;         // window 합이 이 이상이면 문 움직임 시작 (4차: 문 250~480, 사람 75 이하)
    float motion_off = 30.0f;         // window 합이 이 이하로 떨어지면 움직임 끝
    float motion_reverse_speed = 0.10f; // 움직이는 도중 평균 속도가 이 이상으로 반대 방향이면 방향이 바뀐 것으로 봄 [m/s]
                                        // 닫힘 끝의 약한 튕김(평균 0.07 이하)은 무시 (1차 S0-3)

    static DoorStateConfig fromConfig(const Config& cfg);
    bool isValid() const {
        return view_zone.isValid() && window > 0 && close_threshold < open_threshold &&
               (!motion_enabled || (motion_zone.isValid() && motion_off < motion_on));
    }
};

class DoorStateEstimator {
public:
    explicit DoorStateEstimator(const DoorStateConfig& config);

    // 한 프레임(좌표 변환 후 전체 점)을 넣으면 현재 문 상태를 돌려준다
    DoorState update(const Frame& points);

    DoorState state() const { return state_; }
    std::size_t lastCount() const { return last_count_; }     // 이번 프레임의 문 너머 점 수
    float average() const { return average_; }                // 최근 window 프레임 평균 (문 너머)
    std::size_t motionCount() const { return motion_sum_; }   // 최근 window 프레임의 문짝 움직임 점 합
    bool moving() const { return moving_; }                   // 문이 움직이는 중인지

private:
    DoorState updateView(std::size_t n);   // (1) 문 너머 보기로 view_state_ 갱신
    void updateMotion(const Frame& points); // (2) 문짝 움직임으로 state_ 갱신

    DoorStateConfig config_;
    DoorState state_ = DoorState::UNKNOWN;       // 최종 결과

    // (1) 문 너머 보기
    std::deque<std::size_t> counts_;
    DoorState view_state_ = DoorState::UNKNOWN;
    std::size_t last_count_ = 0;
    float average_ = 0.0f;

    // (2) 문짝 움직임
    std::deque<std::pair<std::size_t, float>> motion_;  // 프레임마다 (점 수, 도플러 합)
    std::size_t motion_sum_ = 0;
    bool moving_ = false;        // 지금 문이 움직이는 중
    bool motion_seen_ = false;   // 한 번이라도 움직임을 봤는지 (본 뒤로는 (1)을 안 씀)
    double event_dsum_ = 0.0;    // 이번 움직임 동안 쌓인 도플러 합 (부호로 열림/닫힘).
                                 // 도중에 반대 방향 강한 움직임이 오면 그때부터 새로 쌓음 (닫히다가 다시 열림) // A 리뷰 반영
};

}  // namespace elevsafe::sp