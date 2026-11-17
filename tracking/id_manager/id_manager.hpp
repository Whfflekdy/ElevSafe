// tracking/id_manager.hpp
// 그리디 매칭 기반 ID 관리 + 트랙 생명주기 (TENTATIVE -> CONFIRMED -> LOST -> 삭제).
// 입력: FrameDetections (detection_loader.hpp). 프레임 간 시간 차이는 timestamp_us로 계산한다.
// 칼만 필터: third_party/kalman-cpp 수정본(KalmanFilter)의 6상태 [x,y,z,vx,vy,vz] 모델을 사용한다.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include <Eigen/Dense>
#include "detection/detection_loader.hpp"
#include "third_party/kalman-cpp/kalman.hpp"

enum class TrackState { TENTATIVE, CONFIRMED, LOST };  // 삭제된 트랙은 목록에서 제거됨(removedTracks)

// 아래 기본값은 전부 임시값이다. 승객이 있는 녹화본으로 gate, n_init, max_age_s 등을 정할 것.
// (칼만 관련 기본값은 kalman_track.hpp의 makeTrack과 같은 값)
struct TrackerConfig {
    // 매칭
    double gate_xy = 0.8;               // [m] 수평 거리 게이트
    double gate_z = 1.0;                // [m] 수직 게이트 (FALL 대응으로 xy보다 넉넉하게)
    double z_weight = 0.5;              // 비용 계산에서 z 오차 가중치 (1이면 일반 유클리드)
    // 생명주기
    int    n_init = 3;                  // CONFIRMED까지 필요한 연속 매칭 수
    double max_age_s = 5.0;             // LOST 상태로 기다리는 최대 시간 [s]
    double tentative_max_gap_s = 1.0;   // TENTATIVE가 이 시간 넘게 갱신 없으면 삭제 [s]
    double lost_vel_decay_per_s = 0.3;  // LOST 중 초당 속도 감쇠 비율 (0.3 = 1초에 30%만 남김, 1이면 감쇠 없음)
    // 칼만 필터
    double q_xy = 1.0;                  // 가속도 표준편차 [m/s^2] (프로세스 노이즈, makeQ의 sigma_a)
    double q_z = 1.0;
    double r_xy = 0.15;                 // 측정 표준편차 [m]
    double r_z = 0.15;
    double p0_pos_var = 0.5;            // 초기 공분산: 위치 분산
    double p0_vel_var = 4.0;            // 초기 공분산: 속도 분산
    // 상태 룰용 z 이력
    size_t z_history_len = 50;
};

struct ZSample { int64_t t_us; double z; };

struct Track {
    int internal_id = -1;               // 생성 순서 (TENTATIVE 포함)
    int id = -1;                        // 공개 ID. CONFIRMED가 될 때 부여, 그 전에는 -1
    TrackState state = TrackState::TENTATIVE;
    KalmanFilter kf;                    // 6상태 [x,y,z,vx,vy,vz]
    int hits = 0;                       // 연속 매칭 수 (TENTATIVE 판정용)
    int64_t created_us = 0;
    int64_t last_update_us = 0;         // 마지막으로 검출과 매칭된 시각
    double time_since_update_s = 0;
    int n_points = 0;                   // 마지막 매칭 검출의 점 개수
    std::deque<ZSample> z_history;      // 매칭된 시점의 측정 z (FALL 룰용)

    // kf 상태를 읽기 쉬운 값으로 캐시 (kf.state()가 const가 아니라서). predict/correct 뒤 syncState() 호출.
    double x() const { return pos_[0]; }
    double y() const { return pos_[1]; }
    double z() const { return pos_[2]; }
    double vx() const { return vel_[0]; }
    double vy() const { return vel_[1]; }
    double vz() const { return vel_[2]; }
    void syncState() {
        Eigen::VectorXd s = kf.state();
        if (s.rows() < 6) return;  // 초기화 안 된 필터 방어
        for (int i = 0; i < 3; ++i) { pos_[i] = s(i); vel_[i] = s(i + 3); }
    }

private:
    double pos_[3] = {0, 0, 0};
    double vel_[3] = {0, 0, 0};
};

class TrackManager {
public:
    explicit TrackManager(TrackerConfig cfg = TrackerConfig());

    // 한 프레임 처리. 검출이 0개여도 timestamp를 넣어 호출하면 시간이 흐른 것으로 처리한다.
    const std::vector<Track>& update(const FrameDetections& fd);

    const std::vector<Track>& tracks() const { return tracks_; }            // 모든 상태
    std::vector<Track> confirmedTracks() const;                              // CONFIRMED만
    const std::vector<Track>& removedTracks() const { return removed_; }    // 직전 update에서 삭제된 트랙
    int confirmedIdCount() const { return next_id_ - 1; }                   // 지금까지 부여한 공개 ID 수
    const TrackerConfig& config() const { return cfg_; }
    void reset();

private:
    bool gateCost(const Track& t, const Detection& d, double& cost) const;
    void confirm(Track& t);

    TrackerConfig cfg_;
    std::vector<Track> tracks_;
    std::vector<Track> removed_;
    int next_internal_ = 0;
    int next_id_ = 1;
    int64_t prev_ts_ = 0;
    bool have_prev_ = false;
};
