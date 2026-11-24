// elevsafe_types.hpp -- A/B/C가 같이 쓰는 공통 타입 (헤더 하나, 의존성 없음)
// 인터페이스 합의 v4(11/19) 확정 사항:
//   DetZone  : B의 Zone과 숫자를 똑같이 맞춤 (UNKNOWN=0, DOOR=1, CABIN=2, HALL=3). A가 static_cast로 변환.
//   DoorState: 지금은 OPEN / CLOSED / UNKNOWN 세 가지만 (OPENING/CLOSING 구분 없음).
// 이전에는 cluster_loader.hpp 와 state_machine.hpp 가 DoorState 를 각자 정의해서
// 같은 파일에 같이 include 하면 재정의 오류가 났다. 이제 여기 한 곳에만 둔다.
#pragma once
#include <cstdint>
#include <string>

namespace elevsafe {

enum class DetZone : std::uint8_t { UNKNOWN = 0, DOOR = 1, CABIN = 2, HALL = 3 };

inline DetZone parseZone(const std::string& s) {
    if (s == "DOOR" || s == "1") return DetZone::DOOR;
    if (s == "CABIN" || s == "2") return DetZone::CABIN;
    if (s == "HALL" || s == "3") return DetZone::HALL;
    return DetZone::UNKNOWN;
}

inline const char* toString(DetZone z) {
    switch (z) {
        case DetZone::DOOR: return "DOOR";
        case DetZone::CABIN: return "CABIN";
        case DetZone::HALL: return "HALL";
        default: return "UNKNOWN";
    }
}

enum class DoorState { UNKNOWN, OPEN, CLOSED };

inline DoorState parseDoorState(const std::string& s) {
    if (s == "OPEN") return DoorState::OPEN;
    if (s == "CLOSED") return DoorState::CLOSED;
    return DoorState::UNKNOWN;  // 알 수 없는 문자열도 UNKNOWN (door_command=null 쪽으로 안전)
}

inline const char* toString(DoorState s) {
    switch (s) {
        case DoorState::OPEN: return "OPEN";
        case DoorState::CLOSED: return "CLOSED";
        default: return "UNKNOWN";
    }
}

}  // namespace elevsafe
