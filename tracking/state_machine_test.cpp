#include "state_machine.hpp"
#include <cstdio>
using namespace elevsafe;
static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } }while(0)

static PersonObs P(int id, Zone z, float y, float zt, float dop = 0) {
  PersonObs p; p.id = id; p.zone = z; p.y = y; p.z_top = zt; p.mean_doppler = dop; return p;
}
static Output step(StateMachine& sm, double t, DoorState d, std::vector<PersonObs> v) {
  FrameInput in; in.t = t; in.door = d; in.persons = std::move(v); return sm.update(in);
}

int main() {
  const double dt = 0.1;
  { // 1) 출입: DOOR 통과 중 HOLD → CABIN 진입 후 close_wait 뒤 CLOSE
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 10; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, -0.4f + 0.06f * i, 1.7f)});
    CHECK(o.door_command == DoorCommand::HOLD);
    CHECK(o.tracks[0].phase == Phase::ENTERING);
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.tracks[0].phase == Phase::INSIDE);
    CHECK(o.door_command == DoorCommand::HOLD);          // 대기 중
    for (int i = 0; i < 40; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::CLOSE);
  }
  { // 2) door UNKNOWN → 항상 null
    StateMachine sm; Output o = step(sm, 0, DoorState::UNKNOWN, {P(1, Zone::DOOR, -0.2f, 1.7f)});
    CHECK(o.door_command == DoorCommand::NONE);
  }
  { // 3) CLOSED + 새 EXITING → OPEN 내지 않음
    StateMachine sm; Output o;
    for (int i = 0; i < 10; ++i) o = step(sm, i * dt, DoorState::CLOSED, {P(1, Zone::DOOR, 0.2f - 0.06f * i, 1.7f)});
    CHECK(o.door_command == DoorCommand::NONE);
  }
  { // 4) 캐빈 낙상 → FALL → 40초 → IMMOBILE(문 명령 없음) → 서서 5초 → NORMAL
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    const double fall_t_expected = t;
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::FALL);
    CHECK(o.tracks[0].fall_detect_t >= fall_t_expected);
    CHECK(o.door_command == DoorCommand::NONE);
    for (int i = 0; i < 380; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 0.3f)});
    CHECK(o.safety_state == Safety::FALL);               // 40초 전
    for (int i = 0; i < 40; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 0.3f)});
    CHECK(o.safety_state == Safety::IMMOBILE);
    CHECK(o.door_command == DoorCommand::NONE);
    for (int i = 0; i < 70; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    CHECK(o.safety_state == Safety::NORMAL);
  }
  { // 5) 쪼그려 앉기(느린 하강) → FALL 아님
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    for (int i = 0; i < 30; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.7f * i / 29.0f)});
    for (int i = 0; i < 30; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.0f)});
    CHECK(o.safety_state == Safety::NORMAL);
  }
  { // 6) 출입구 낙상: HOLD 유지, hold_max 초과해도 해제 안 함, 문 열림 50초 후 관제 경보
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, -0.4f + 0.04f * i, 1.7f)});
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, 0.0f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::FALL);
    // 문 열림 50초(= door_open_max 60초 - 경고 여유 10초) 이상이 되어야 관제 경보가 켜진다: 25프레임 + 600프레임 = 62.5초
    for (int i = 0; i < 600; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, 0.0f, 0.3f)});
    CHECK(o.door_command == DoorCommand::HOLD);
    CHECK(o.hold_exceeded);
    CHECK(o.door_open_limit_warning);
  }
  { // 7) 추적 1~2초 끊김은 사람 있음으로 간주(HOLD 유지)
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 10; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, -0.4f + 0.06f * i, 1.7f)});
    for (int i = 0; i < 15; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {});
    CHECK(o.door_command == DoorCommand::HOLD);
    for (int i = 0; i < 10; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {});
    CHECK(o.tracks.empty());
  }
  { // 8) CLOSE 는 문이 CLOSED 가 될 때까지 유지, 닫히는 중 다시 점유되면 HOLD 복귀
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 10; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, -0.4f + 0.06f * i, 1.7f)});
    for (int i = 0; i < 70; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::CLOSE);
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::CLOSE);          // 문이 아직 열려 있으면 계속 CLOSE
    o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f), P(2, Zone::DOOR, 0.1f, 1.7f)});
    CHECK(o.door_command == DoorCommand::HOLD);           // 닫는 중 새 승객이 문턱에 오면 CLOSE 취소
    t += dt;
    o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::NONE);           // 닫힘 확인되면 종료
    for (int i = 0; i < 25; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.5f, 1.7f)});  // 2번 승객 끊김 2초 경과
    o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::NONE);           // 다시 열려도 우리가 낸 명령 없음 (정상 운행)
  }
  { // 9) door UNKNOWN 이 되면 CLOSE 유지도 중단 (null)
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 10; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::DOOR, -0.4f + 0.06f * i, 1.7f)});
    for (int i = 0; i < 70; ++i, t += dt) o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::CLOSE);
    o = step(sm, t, DoorState::UNKNOWN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::NONE);
    t += dt;
    o = step(sm, t, DoorState::OPEN, {P(1, Zone::CABIN, 0.5f, 1.7f)});
    CHECK(o.door_command == DoorCommand::NONE);
  }
  { // 10) 낙상 승객이 추적에서 끊겨도 5초까지는 FALL 유지, 이후 정리. 일반 승객은 2초
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::FALL);
    for (int i = 0; i < 45; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {});
    CHECK(o.safety_state == Safety::FALL);                // 약 4.5초 끊김: 아직 유지
    CHECK(!o.tracks.empty() && o.tracks[0].safety == Safety::FALL);
    for (int i = 0; i < 10; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {});
    CHECK(o.tracks.empty() && o.safety_state == Safety::NORMAL);   // 5초 넘으면 정리
  }
  { // 11) 끊겼다가 같은 ID 로 돌아오면 FALL 상태 이어짐
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.28f * (i + 1))});
    for (int i = 0; i < 30; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {});
    o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 0.3f)});
    CHECK(o.safety_state == Safety::FALL);
  }
  { // 12) reset(): 모든 상태 초기화
    StateMachine sm; double t = 0; Output o;
    for (int i = 0; i < 20; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::FALL);
    sm.reset();
    o = step(sm, t, DoorState::CLOSED, {});
    CHECK(o.tracks.empty() && o.safety_state == Safety::NORMAL && o.door_command == DoorCommand::NONE);
  }
  { // 13) HALL 에서는 FALL/IMMOBILE 판정 안 함 (hall_enabled=true 여도). 승강장에서 캐빈으로 들어오면 baseline 을 새로 잡는다
    Config c; c.hall_enabled = true; StateMachine sm(c); double t = 0; Output o;
    for (int i = 0; i < 30; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::HALL, -1.5f, 1.7f)});
    CHECK(o.tracks[0].phase == Phase::OUTSIDE);
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::HALL, -1.5f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::NORMAL);              // 승강장 낙상 급 하강 -> FALL 아님
    for (int i = 0; i < 500; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::HALL, -1.5f, 0.3f)});
    CHECK(o.safety_state == Safety::NORMAL);              // 50초 누워 있어도 IMMOBILE 아님
    for (int i = 0; i < 40; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    CHECK(o.safety_state == Safety::NORMAL);              // 캐빈으로 오면 정상 (누운 높이가 baseline 으로 남지 않음)
    for (int i = 0; i < 5; ++i, t += dt) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::FALL);                // 캐빈에서는 정상적으로 FALL 판정
  }
  { // 14) 누운 동안 덩어리가 최대 0.46초(9프레임 @20Hz)씩 반복해서 끊겨도 같은 ID 로 FALL -> 40초 뒤 IMMOBILE
    StateMachine sm; const double d = 0.05; double t = 0; Output o; int n = 0;
    for (int i = 0; i < 60; ++i, t += d) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f)});
    for (int i = 0; i < 5; ++i, t += d) o = step(sm, t, DoorState::CLOSED, {P(1, Zone::CABIN, 0.6f, 1.7f - 0.28f * (i + 1))});
    CHECK(o.safety_state == Safety::FALL);
    const double t_fall = t;
    bool immobile_early = false;
    while (t - t_fall < 39.5) {                           // 31 프레임 검출 + 9 프레임 끊김 반복
      bool gap = (n % 40) >= 31;
      o = step(sm, t, DoorState::CLOSED, gap ? std::vector<PersonObs>{} : std::vector<PersonObs>{P(1, Zone::CABIN, 0.6f, 0.3f)});
      if (o.safety_state == Safety::IMMOBILE) immobile_early = true;
      t += d; ++n;
    }
    CHECK(!immobile_early && o.safety_state != Safety::NORMAL);   // 40초 전: FALL 유지, 상태 안 사라짐
    for (int i = 0; i < 60; ++i, t += d, ++n) {
      bool gap = (n % 40) >= 31;
      o = step(sm, t, DoorState::CLOSED, gap ? std::vector<PersonObs>{} : std::vector<PersonObs>{P(1, Zone::CABIN, 0.6f, 0.3f)});
    }
    CHECK(o.safety_state == Safety::IMMOBILE);            // 끊김이 있어도 IMMOBILE 확정
    CHECK(!o.tracks.empty() && o.tracks[0].id == 1);
  }
  std::printf(fails ? "FAILED %d\n" : "ALL PASSED\n", fails);
  return fails != 0;
}
