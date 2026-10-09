// radar_point.h
// 파이프라인 전체에서 공유하는 4D 레이더 포인트 자료구조
#pragma once 

#include <cstdint>
#include <vector>

// 포인트가 속한 감지 영역
enum class Zone: std::uint8_t{
    NONE = 0,  // ROI 밖 (제거 대상) 
    DOOR = 1,  // 출입구 영역
    CABIN = 2  // 엘리베이터 내부 영역
};

struct RadarPoint{
    float x=0.0f;       // 좌우 [m]
    float y=0.0f;       // 깊이 [m]
    float z=0.0f;       // 높이 [m]
    float doppler=0.0f; // 시선 방향 속도 [m/s]
    float power=0.0f;   // 수신 신호 세기

    int target_id = -1; // 센서가 부여한 ID(-1: 부여되지 않음)
    Zone zone = Zone::NONE; // ROI 필터가 채워 넣는 영역 라벨. 

    // 센서 기준 극좌표 격자 번호 (좌표 변환 전에 PolarGrid가 계산)
    bool has_bins = false; // 격좌 좌표 계산 여부 (계산 안 됨을 표시하기 위해)
    int range_bin = 0; // 거리 칸 번호
    int u_bin = 0;     // 좌우 사인 칸 번호(음수 가능)
    int v_bin = 0; // 위아래 사인 칸 번호 (음수 가능)

    int cluster_id = -1; // DBSCAN 클러스터링 번호 (-1: 노이즈 또는 클러스터링 전)
};

// 한 프레임에 들어온 포인트 묶음(Frame 별칭)
using Frame = std::vector<RadarPoint>;

// 한 프레임의 메타 정보 + 포인트 목록
// (파일 입출력과 main 에서 사용. 필터들은 기존처럼 Frame 만 받는다)
struct FrameData {
    int frame_id = -1;          // 센서 원본 frameCount
    double timestamp_ms = 0.0;  // 세션 첫 프레임 기준 경과 시간 [ms]
    Frame points;               // 포인트 목록
};

// 헤더에 함수 본문을 두면 include한 .cpp마다 정의가 생기므로
// inline으로 표시해 링크 시 중복 정의 에러를 막음.
inline const char* zoneToString(Zone zone){
    switch(zone){
        case Zone::NONE:
            return "NONE";
            break;
        case Zone::DOOR:
            return "DOOR";
            break;
        case Zone::CABIN:
            return "CABIN";
            break;
    }
    return "UNKNOWN"; // 값이 잘못 들어온 경우 대비. 
}