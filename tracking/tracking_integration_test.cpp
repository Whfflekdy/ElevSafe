// TrackingEngine::processFrame() 통합 테스트: B 덩어리(Detection) -> 추적 -> 상태 판단 -> door_command
// 합성 데이터. 좌표 규칙은 인터페이스 v4 (Y 캐빈 안쪽 +, 원점 = 문턱 띠 가운데), z_top 은 바닥 기준 높이.
#include <cmath>
#include <cstdio>
#include "tracking_api.hpp"

using elevsafe::DetZone;
using elevsafe::DoorCommand;
using elevsafe::DoorState;
using elevsafe::Phase;
using elevsafe::Safety;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("  [PASS] %s\n", msg); \
    else { std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } } while (0)

static Detection det(double x, double y, double z_top, DetZone zone) {
    Detection d;
    d.cx = x; d.cy = y; d.cz = z_top * 0.6;     // 평균 높이는 키의 약 60%로 가정
    d.zmin = 0.1; d.zmax = z_top; d.z_top = z_top;
    d.n_points = 60; d.zone = zone;
    return d;
}
static FrameDetections frame(int f, std::vector<Detection> ds) {
    FrameDetections fd; fd.frame = f; fd.timestamp_us = (int64_t)f * 100000;  // 10 Hz
    fd.detections = std::move(ds); return fd;
}
static const TrackOutput* find(const FrameResult& r, int id) {
    for (const auto& t : r.tracks) if (t.id == id) return &t;
    return nullptr;
}

// 1) hall_enabled=false 이면 HALL 검출은 트랙을 만들지 않는다 / true 이면 만든다
static void testHallFilter() {
    std::printf("[1] hall_enabled 필터\n");
    {
        TrackingEngine e;                       // 기본 hall_enabled=false
        FrameResult r;
        for (int f = 0; f < 20; ++f) r = e.processFrame(frame(f, {det(0.5, -1.0, 1.7, DetZone::HALL)}), DoorState::OPEN);
        CHECK(e.manager().confirmedIdCount() == 0 && r.tracks.empty(), "꺼짐: 승강장 사람은 ID가 발급되지 않음");
        CHECK(r.door_command == DoorCommand::NONE, "꺼짐: door_command null");
    }
    {
        TrackingEngine e; e.setHallEnabled(true);
        FrameResult r;
        for (int f = 0; f < 20; ++f) r = e.processFrame(frame(f, {det(0.5, -1.0, 1.7, DetZone::HALL)}), DoorState::OPEN);
        CHECK(e.manager().confirmedIdCount() == 1 && r.tracks.size() == 1, "켜짐: 승강장 사람도 추적");
        CHECK(!r.tracks.empty() && r.tracks[0].phase == Phase::OUTSIDE, "켜짐: Phase = OUTSIDE");
    }
}

// 2) 탑승: 문턱 통과 중 HOLD -> 캐빈 안 3초 대기 후 CLOSE (문이 닫힐 때까지 유지)
static void testEnterAndClose() {
    std::printf("[2] 탑승 -> HOLD -> CLOSE\n");
    TrackingEngine e;
    FrameResult r; int f = 0;
    for (int i = 0; i < 12; ++i, ++f) r = e.processFrame(frame(f, {det(0, -0.3 + 0.06 * i, 1.7, DetZone::DOOR)}), DoorState::OPEN);
    const TrackOutput* t = r.tracks.empty() ? nullptr : &r.tracks[0];
    CHECK(t && t->phase == Phase::ENTERING, "문턱 통과 중 Phase = ENTERING");
    CHECK(r.door_command == DoorCommand::HOLD, "문턱 점유 중 door_command = HOLD");
    for (int i = 0; i < 20; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::OPEN);
    CHECK(!r.tracks.empty() && r.tracks[0].phase == Phase::INSIDE, "캐빈 안 Phase = INSIDE");
    CHECK(r.door_command == DoorCommand::HOLD, "통과 직후 close_wait 동안 HOLD 유지");
    for (int i = 0; i < 40; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::OPEN);
    CHECK(r.door_command == DoorCommand::CLOSE, "대기 후 CLOSE");
    for (int i = 0; i < 10; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::OPEN);
    CHECK(r.door_command == DoorCommand::CLOSE, "문이 열려 있는 동안 CLOSE 유지");
    r = e.processFrame(frame(f++, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::CLOSED);
    CHECK(r.door_command == DoorCommand::NONE, "CLOSED 확인 후 null");
    r = e.processFrame(frame(f++, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::UNKNOWN);
    CHECK(r.door_command == DoorCommand::NONE, "door UNKNOWN 이면 null");
}

// 3) 캐빈 낙상 -> FALL -> 40초 -> IMMOBILE (문 명령 없음), 서서 5초 -> NORMAL
static void testFallImmobile() {
    std::printf("[3] 낙상 -> 부동 -> 회복\n");
    TrackingEngine e;
    FrameResult r; int f = 0;
    for (int i = 0; i < 20; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::CLOSED);
    int id = r.tracks.empty() ? -1 : r.tracks[0].id;
    CHECK(id > 0, "서 있는 승객에게 공개 ID 발급");
    for (int i = 0; i < 5; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7 - 0.28 * (i + 1), DetZone::CABIN)}), DoorState::CLOSED);
    const TrackOutput* t = find(r, id);
    CHECK(r.safety_state == Safety::FALL && t && t->safety == Safety::FALL, "z_top 급강하 -> FALL, 같은 ID 유지");
    CHECK(r.door_command == DoorCommand::NONE, "캐빈 안 낙상은 문 명령 없음");
    for (int i = 0; i < 380; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 0.3, DetZone::CABIN)}), DoorState::CLOSED);
    CHECK(r.safety_state == Safety::FALL, "40초 전에는 FALL");
    for (int i = 0; i < 40; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 0.3, DetZone::CABIN)}), DoorState::CLOSED);
    t = find(r, id);
    CHECK(r.safety_state == Safety::IMMOBILE && t && t->safety == Safety::IMMOBILE, "40초 정지 -> IMMOBILE");
    for (int i = 0; i < 70; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::CLOSED);
    CHECK(r.safety_state == Safety::NORMAL, "서서 5초 이상 -> NORMAL");
}

// 4) 낙상 후 검출이 4.5초 끊겼다가 돌아와도 같은 ID, FALL 이어짐 / LOST 트랙도 결과에 포함
static void testLostDuringFall() {
    std::printf("[4] 낙상 중 추적 끊김\n");
    TrackingEngine e;
    FrameResult r; int f = 0;
    for (int i = 0; i < 20; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::CLOSED);
    int id = r.tracks.empty() ? -1 : r.tracks[0].id;
    for (int i = 0; i < 5; ++i, ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7 - 0.28 * (i + 1), DetZone::CABIN)}), DoorState::CLOSED);
    for (int i = 0; i < 45; ++i, ++f) r = e.processFrame(frame(f, {}), DoorState::CLOSED);   // 검출 없음 4.5초
    const TrackOutput* t = find(r, id);
    CHECK(t && t->lost && t->safety == Safety::FALL, "끊긴 동안 LOST 트랙이 결과에 남고 FALL 유지");
    CHECK(r.safety_state == Safety::FALL, "전체 safety_state 도 FALL 유지");
    r = e.processFrame(frame(f++, {det(0, 0.6, 0.3, DetZone::CABIN)}), DoorState::CLOSED);
    t = find(r, id);
    CHECK(t && !t->lost && t->safety == Safety::FALL, "돌아오면 같은 ID, FALL 이어짐");
}

// 5) reset(): ID 번호와 상태가 모두 초기화
static void testReset() {
    std::printf("[5] reset()\n");
    TrackingEngine e;
    FrameResult r;
    for (int f = 0; f < 20; ++f) r = e.processFrame(frame(f, {det(0, 0.6, 1.7, DetZone::CABIN)}), DoorState::CLOSED);
    CHECK(e.manager().confirmedIdCount() == 1, "리셋 전 ID 1개 발급");
    e.reset();
    r = e.processFrame(frame(0, {}), DoorState::CLOSED);
    CHECK(r.tracks.empty() && r.safety_state == Safety::NORMAL && e.manager().confirmedIdCount() == 0, "리셋 후 트랙/상태/ID 카운터 초기화");
}

int main() {
    testHallFilter();
    testEnterAndClose();
    testFallImmobile();
    testLostDuringFall();
    testReset();
    std::printf(g_fail ? "\n실패 %d건\n" : "\n전부 통과\n", g_fail);
    return g_fail ? 1 : 0;
}
