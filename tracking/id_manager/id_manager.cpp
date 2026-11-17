#include "id_manager/id_manager.hpp"

#include <algorithm>
#include <cmath>

#include "kalman_track.hpp"  // makeA

namespace {

// 6x6 전이 행렬. vel_decay < 1이면 속도 성분을 감쇠시킨다(LOST 중 사용). 1이면 makeA와 같다.
Eigen::MatrixXd buildA(double dt, double vel_decay) {
    Eigen::MatrixXd A = makeA(dt);
    for (int i = 3; i < 6; ++i) A(i, i) = vel_decay;
    return A;
}

// 프로세스 노이즈. makeQ와 같은 형태인데 xy/z의 sigma_a를 따로 줄 수 있다.
Eigen::MatrixXd buildQ(double dt, double sigma_xy, double sigma_z) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(6, 6);
    const double q11 = dt * dt * dt * dt / 4.0, q12 = dt * dt * dt / 2.0, q22 = dt * dt;
    for (int i = 0; i < 3; ++i) {
        const double s2 = (i < 2 ? sigma_xy : sigma_z) * (i < 2 ? sigma_xy : sigma_z);
        Q(i, i) = q11 * s2;
        Q(i, i + 3) = q12 * s2;
        Q(i + 3, i) = q12 * s2;
        Q(i + 3, i + 3) = q22 * s2;
    }
    return Q;
}

KalmanFilter makeFilter(const TrackerConfig& cfg, double t0_s, double x, double y, double z) {
    Eigen::MatrixXd C = Eigen::MatrixXd::Zero(3, 6);
    for (int i = 0; i < 3; ++i) C(i, i) = 1.0;
    Eigen::MatrixXd R = Eigen::MatrixXd::Zero(3, 3);
    R(0, 0) = cfg.r_xy * cfg.r_xy;
    R(1, 1) = cfg.r_xy * cfg.r_xy;
    R(2, 2) = cfg.r_z * cfg.r_z;
    Eigen::MatrixXd P = Eigen::MatrixXd::Zero(6, 6);
    for (int i = 0; i < 3; ++i) { P(i, i) = cfg.p0_pos_var; P(i + 3, i + 3) = cfg.p0_vel_var; }
    KalmanFilter kf(0.0, buildA(0.0, 1.0), C, buildQ(0.0, cfg.q_xy, cfg.q_z), R, P);
    Eigen::VectorXd x0(6);
    x0(0) = x; x0(1) = y; x0(2) = z; x0(3) = 0; x0(4) = 0; x0(5) = 0;
    kf.init(t0_s, x0);
    return kf;
}

}  // namespace

// ---------- TrackManager ----------
TrackManager::TrackManager(TrackerConfig cfg) : cfg_(cfg) {}

void TrackManager::reset() {
    tracks_.clear();
    removed_.clear();
    next_internal_ = 0;
    next_id_ = 1;
    have_prev_ = false;
}

std::vector<Track> TrackManager::confirmedTracks() const {
    std::vector<Track> out;
    for (const Track& t : tracks_)
        if (t.state == TrackState::CONFIRMED) out.push_back(t);
    return out;
}

void TrackManager::confirm(Track& t) {
    t.state = TrackState::CONFIRMED;
    if (t.id < 0) t.id = next_id_++;
}

bool TrackManager::gateCost(const Track& t, const Detection& d, double& cost) const {
    const double dx = d.cx - t.x(), dy = d.cy - t.y(), dz = d.cz - t.z();
    const double dxy = std::hypot(dx, dy);
    if (dxy > cfg_.gate_xy || std::fabs(dz) > cfg_.gate_z) return false;  // 게이팅
    const double wz = cfg_.z_weight * dz;
    cost = std::sqrt(dxy * dxy + wz * wz);
    return true;
}

const std::vector<Track>& TrackManager::update(const FrameDetections& fd) {
    removed_.clear();
    const int64_t ts = fd.timestamp_us;
    double dt = have_prev_ ? (ts - prev_ts_) * 1e-6 : 0.0;
    if (dt < 0) dt = 0;  // 시간이 거꾸로 온 경우 방어
    prev_ts_ = ts;
    have_prev_ = true;

    // 1. 예측 (LOST는 전이 행렬에서 속도를 감쇠시킴: 사라진 사람이 계속 멀어지는 것을 막음)
    for (Track& t : tracks_) {
        const double f = (t.state == TrackState::LOST) ? std::pow(cfg_.lost_vel_decay_per_s, dt) : 1.0;
        t.kf.predict(dt, buildA(dt, f), buildQ(dt, cfg_.q_xy, cfg_.q_z));
        t.syncState();
        t.time_since_update_s = (ts - t.last_update_us) * 1e-6;
    }

    // 2. 만료된 트랙 삭제 (매칭 전에 처리)
    {
        std::vector<Track> kept;
        for (Track& t : tracks_) {
            const bool expired =
                (t.state == TrackState::LOST && t.time_since_update_s > cfg_.max_age_s) ||
                (t.state == TrackState::TENTATIVE && t.time_since_update_s > cfg_.tentative_max_gap_s);
            if (expired) removed_.push_back(t); else kept.push_back(std::move(t));
        }
        tracks_ = std::move(kept);
    }

    // 3~4. 거리표 + 게이팅 + 그리디 선택 (CONFIRMED -> LOST -> TENTATIVE 순으로 단계별 매칭)
    const std::vector<Detection>& dets = fd.detections;
    std::vector<int> track_to_det(tracks_.size(), -1), det_to_track(dets.size(), -1);
    struct Pair { double cost; int ti, di; };
    for (TrackState stage : {TrackState::CONFIRMED, TrackState::LOST, TrackState::TENTATIVE}) {
        std::vector<Pair> pairs;
        for (int ti = 0; ti < (int)tracks_.size(); ++ti) {
            if (tracks_[ti].state != stage) continue;
            for (int di = 0; di < (int)dets.size(); ++di) {
                if (det_to_track[di] != -1) continue;
                double c;
                if (gateCost(tracks_[ti], dets[di], c)) pairs.push_back({c, ti, di});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            if (a.cost != b.cost) return a.cost < b.cost;
            if (a.ti != b.ti) return a.ti < b.ti;
            return a.di < b.di;
        });
        for (const Pair& p : pairs) {
            if (track_to_det[p.ti] != -1 || det_to_track[p.di] != -1) continue;
            track_to_det[p.ti] = p.di;
            det_to_track[p.di] = p.ti;
        }
    }

    // 5-a. 매칭된 쌍 -> 칼만 update + 상태 전환
    for (int ti = 0; ti < (int)tracks_.size(); ++ti) {
        const int di = track_to_det[ti];
        if (di < 0) continue;
        Track& t = tracks_[ti];
        const Detection& d = dets[di];
        Eigen::VectorXd meas(3);
        meas(0) = d.cx; meas(1) = d.cy; meas(2) = d.cz;
        t.kf.correct(meas);
        t.syncState();
        t.last_update_us = ts;
        t.time_since_update_s = 0;
        t.n_points = d.n_points;
        t.z_history.push_back({ts, d.cz});
        while (t.z_history.size() > cfg_.z_history_len) t.z_history.pop_front();
        if (t.state == TrackState::LOST) {
            t.state = TrackState::CONFIRMED;
        } else if (t.state == TrackState::TENTATIVE) {
            if (++t.hits >= cfg_.n_init) confirm(t);
        }
    }

    // 5-b. 매칭 안 된 트랙: TENTATIVE는 삭제, CONFIRMED는 LOST (LOST는 그대로)
    {
        std::vector<Track> kept;
        for (int ti = 0; ti < (int)tracks_.size(); ++ti) {
            Track& t = tracks_[ti];
            if (track_to_det[ti] < 0) {
                if (t.state == TrackState::TENTATIVE) { removed_.push_back(t); continue; }
                if (t.state == TrackState::CONFIRMED) t.state = TrackState::LOST;
            }
            kept.push_back(std::move(t));
        }
        tracks_ = std::move(kept);
    }

    // 5-c. 매칭 안 된 검출 -> 새 TENTATIVE 트랙
    for (int di = 0; di < (int)dets.size(); ++di) {
        if (det_to_track[di] != -1) continue;
        const Detection& d = dets[di];
        Track t;
        t.internal_id = next_internal_++;
        t.state = TrackState::TENTATIVE;
        t.hits = 1;
        t.created_us = t.last_update_us = ts;
        t.n_points = d.n_points;
        t.kf = makeFilter(cfg_, ts * 1e-6, d.cx, d.cy, d.cz);
        t.syncState();
        t.z_history.push_back({ts, d.cz});
        if (t.hits >= cfg_.n_init) confirm(t);
        tracks_.push_back(std::move(t));
    }
    return tracks_;
}
