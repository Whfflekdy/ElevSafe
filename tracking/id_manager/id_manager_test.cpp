// 합성 데이터 테스트: 교차 / 한 프레임 놓침 / FALL(z 급강하) / 단발 노이즈
#include <cmath>
#include <cstdio>
#include <random>
#include "id_manager/id_manager.hpp"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("  [PASS] %s\n", msg); \
    else { std::printf("  [FAIL] %s\n", msg); ++g_fail; } } while (0)

static Detection det(double x, double y, double z) {
    Detection d; d.cx = x; d.cy = y; d.cz = z; d.n_points = 10; return d;
}
static FrameDetections frame(int f, std::vector<Detection> ds) {
    FrameDetections fd; fd.frame = f; fd.timestamp_us = (int64_t)f * 100000;  // 10 Hz
    fd.detections = std::move(ds); return fd;
}
// 가장 가까운 CONFIRMED/LOST 트랙의 공개 id (없으면 -1)
static int nearestId(const TrackManager& m, double x, double y) {
    int best = -1; double bd = 1e9;
    for (const Track& t : m.tracks()) {
        if (t.id < 0) continue;
        double d = std::hypot(t.x() - x, t.y() - y);
        if (d < bd) { bd = d; best = t.id; }
    }
    return best;
}

static void testCrossing() {
    std::printf("[1] 두 사람 교차 (y 간격 0.3m, 노이즈 3cm)\n");
    TrackManager m;
    std::mt19937 rng(1);
    std::normal_distribution<double> n(0, 0.03);
    int idA0 = -1, idB0 = -1;
    for (int f = 0; f < 40; ++f) {
        double ax = -2 + 0.1 * f, bx = 2 - 0.1 * f;
        m.update(frame(f, {det(ax + n(rng), 3.0 + n(rng), 1.0 + n(rng)),
                           det(bx + n(rng), 3.3 + n(rng), 1.0 + n(rng))}));
        if (f == 5) { idA0 = nearestId(m, ax, 3.0); idB0 = nearestId(m, bx, 3.3); }
    }
    int idA1 = nearestId(m, -2 + 0.1 * 39, 3.0), idB1 = nearestId(m, 2 - 0.1 * 39, 3.3);
    CHECK(idA0 > 0 && idB0 > 0 && idA0 != idB0, "초기에 서로 다른 ID 2개 확정");
    CHECK(idA1 == idA0 && idB1 == idB0, "교차 후에도 ID 유지 (스위칭 없음)");
    CHECK(m.confirmedIdCount() == 2, "공개 ID가 총 2개만 발급됨");
}

static void testMissedFrames() {
    std::printf("[2] 검출 놓침 (frame 10~11)\n");
    {
        TrackManager m;
        int id0 = -1; bool sawLost = false;
        for (int f = 0; f < 30; ++f) {
            std::vector<Detection> ds;
            if (f != 10 && f != 11) ds.push_back(det(0.5 * 0.1 * f, 4.0, 1.0));
            m.update(frame(f, ds));
            if (f == 5) id0 = nearestId(m, 0.25, 4.0);
            if (f == 11 && !m.tracks().empty() && m.tracks()[0].state == TrackState::LOST) sawLost = true;
        }
        CHECK(sawLost, "놓친 동안 CONFIRMED -> LOST");
        CHECK(m.tracks().size() == 1 && m.tracks()[0].state == TrackState::CONFIRMED, "다시 잡히면 CONFIRMED 복귀");
        CHECK(m.tracks().size() == 1 && m.tracks()[0].id == id0 && m.confirmedIdCount() == 1, "같은 ID 유지");
    }
    {
        TrackerConfig c; c.max_age_s = 0.15;  // 0.3초 놓치면 삭제되도록
        TrackManager m(c);
        bool removed = false;
        for (int f = 0; f < 30; ++f) {
            std::vector<Detection> ds;
            if (f < 10 || f > 12) ds.push_back(det(0.05 * f, 4.0, 1.0));
            m.update(frame(f, ds));
            if (!m.removedTracks().empty()) removed = true;
        }
        CHECK(removed, "max_age 초과 시 LOST 트랙 삭제 (removedTracks에 기록)");
        CHECK(m.confirmedIdCount() == 2, "재등장 시 새 ID 부여 (삭제 후 재연결은 안 됨)");
    }
}

static void testFall(double gate_z, bool expectSameId, const char* title) {
    std::printf("%s\n", title);
    TrackerConfig c; c.gate_z = gate_z;
    TrackManager m(c);
    for (int f = 0; f < 30; ++f) {
        double z = f < 15 ? 1.0 : (f == 15 ? 0.65 : 0.3);  // 2프레임 만에 1.0 -> 0.3
        m.update(frame(f, {det(1.0, 4.0, z)}));
    }
    if (expectSameId) {
        CHECK(m.confirmedIdCount() == 1, "z 급강하에도 같은 ID 유지");
        CHECK(m.tracks().size() == 1 && m.tracks()[0].state == TrackState::CONFIRMED, "트랙 CONFIRMED 유지");
        CHECK(!m.tracks().empty() && std::fabs(m.tracks()[0].z() - 0.3) < 0.1, "트랙 z가 새 높이로 수렴");
    } else {
        CHECK(m.confirmedIdCount() >= 2, "gate_z가 좁으면 쓰러진 사람이 새 ID로 바뀜 (대조군)");
    }
}

static double lostPositionAfterWalk(double decay) {
    TrackerConfig c; c.lost_vel_decay_per_s = decay;
    TrackManager m(c);
    for (int f = 0; f < 35; ++f) {
        std::vector<Detection> ds;
        if (f < 15) ds.push_back(det(0.1 * f, 4.0, 1.0));  // 1 m/s로 걷다가 15프레임부터 안 보임 (2초)
        m.update(frame(f, ds));
    }
    return m.tracks().empty() ? -1e9 : m.tracks()[0].x();
}

static void testLostVelocityDamping() {
    std::printf("[2c] LOST 중 속도 감쇠 (1 m/s로 걷다 2초간 소실)\n");
    double damped = lostPositionAfterWalk(0.3), undamped = lostPositionAfterWalk(1.0);
    std::printf("      소실 후 예측 x: 감쇠 %.2f m / 감쇠 없음 %.2f m\n", damped, undamped);
    CHECK(damped < undamped - 0.5, "감쇠를 켜면 소실된 트랙이 덜 멀리 나감");
}

static void testSpuriousDetection() {
    std::printf("[4] 단발 노이즈 검출\n");
    TrackManager m;
    for (int f = 0; f < 10; ++f) {
        std::vector<Detection> ds;
        if (f == 4) ds.push_back(det(-1.0, 6.0, 0.5));
        m.update(frame(f, ds));
    }
    CHECK(m.confirmedIdCount() == 0 && m.tracks().empty(), "1프레임짜리 검출은 CONFIRMED가 되지 않고 사라짐");
}

int main() {
    testCrossing();
    testMissedFrames();
    testLostVelocityDamping();
    testFall(1.0, true, "[3a] FALL: gate_z = 1.0 (기본)");
    testFall(0.1, false, "[3b] FALL 대조군: gate_z = 0.1");
    testSpuriousDetection();
    std::printf(g_fail ? "\n실패 %d건\n" : "\n전부 통과\n", g_fail);
    return g_fail ? 1 : 0;
}
