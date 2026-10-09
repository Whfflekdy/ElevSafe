// coordinate_transform.h
// 레이더는 자기 자신을 원점으로 좌표를 주기 때문에
// 엘리베이터 좌표계를 정해두고 모든 점을 그쪽으로 옮긴 뒤 ROI를 검사해야 함.

/*
               (승강장, y < 0)
  (x>0) ─────────────●───────────── (x<0)   ← 문턱 선 (y = 0)
                    원점
              (캐빈 안쪽, y > 0)

        ● 센서 위치
*/
// 원점: 문턱 중앙의 바닥 (z=0)
// x: 승강장에서 캐빈을 바라볼 때 원점(0)부터 오른쪽이 + / y: 문턱(0)부터 캐빈 안쪽이 +/ z: 바닥(0)부터 위쪽이 +

//  ---------- 센서 설치 자세 ----------
//  Retina-4SN 축: 정면 +Y, 오른쪽 +X, 위 +Z (미니테스트 확인)
//  - 아래로 숙여 설치: roll_deg 음수 (예: -25)
//  - 뒷벽에서 문을 향해 설치: yaw_deg = 180

// 센서 좌표계에서 엘리베이터 좌표계로 아래와 같이 변환 가능하다. 
// p_엘리베이터 = R * p_센서 + t  (회전 + 이동)
// R: 센서가 기울어진 정도(roll, pitch, yaw)를 회전행렬로 변환한 것
// - 회전은 축별 각도 세 개로 표현
//    - roll : x축 회전(옆으로 갸웃)
//    - pitch: y축 회전(위아래로 끄덕(센서를 숙인 각도))
//    - yaw  : z축 회전(좌우로 도리도리)
// => R = Rz(yaw) * Rz(pitch) * Rx(roll)
// t: 엘리베이터 원점(문턱 중앙 바닥)에서 본 센서의 위치
//         만일 센서가 문에서 1.4m 안쪽, 바닥에서 2.2m 높이, 승강장에서 들어갈 때 기준으로 좌우 중앙에서 0.3m 왼쪽에 설치되어 있다면
//         t = ( -0.3, 1.4, 2.2 )
// 세 회전을 곱한 회전행렬 R을 만들고, 센서 위치 t를 더해주면 됨. (R은 3x3, t는 3x1)

#pragma once

#include "common/radar_point.h"
#include "common/config.h"

// 센서의 설치 위치와 자세(엘리베이터 좌표계 기준)
//
// [회전 부호 규칙] 이 코드는 R = Rz(yaw) * Ry(pitch) * Rx(roll) 을 사용한다.
//   - 센서 정면이 +x 축인 경우: pitch_deg 가 양수면 센서를 아래로 숙인 것
//     (검증: pitch 30 일 때 정면 1m 점 (1,0,0) -> (0.866, 0, -0.5))
//   - 센서 정면이 +y 축인 경우 (TI mmWave 출력에서 흔함): 숙이는 회전은 roll 이며,
//     아래로 숙이면 roll_deg 가 음수
//
// [실측 전 확인 절차]
//   1. SDK 문서 또는 데이터로 센서 정면이 +x 인지 +y 인지 확인
//      (센서 정면에 사람이 섰을 때 x, y 중 어느 값이 크게 나오는지)
//   2. scratch 테스트로 "정면 1m 점이 변환 후 z 가 내려가는지" 확인해 부호 결정
struct SensorPose{
    // 센서 설치 위치 [m]
    float tx = 0.0f; // x[m] (좌우)
    float ty = 0.0f; // y[m] (앞뒤)
    float tz = 0.0f; // z[m] (높이)

    // 회전 각도 [deg]
    float roll_deg  = 0.0f; // x축 회전(옆으로 갸웃)
    float pitch_deg = 0.0f; // y축 회전(위아래로 끄덕(센서를 숙인 각도))
    float yaw_deg   = 0.0f; // z축 회전(좌우로 도리도리)

    static SensorPose fromConfig(const Config& cfg);  // 설정 파일 값을 ROI와 센서 자세에 추가하는 기능 
};

class CoordinateTransform {
public:
    explicit CoordinateTransform(const SensorPose& pose);

    void apply(RadarPoint& p) const; // 점 하나 변환
    void apply(Frame& points) const; // 프레임 전체 변환

private:
    float R_[3][3]; // 회전행렬
    float t_[3];    // 이동벡터    
};