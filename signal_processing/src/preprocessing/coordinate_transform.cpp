#include "preprocessing/coordinate_transform.h"
#include <cmath>

namespace { // 익명 네임스페이스: 이 파일 안에서만 쓰이는 상수 정의
    //
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f; // degree -> radian 변환 상수
}

SensorPose SensorPose::fromConfig(const Config& cfg){
    SensorPose pose;
    pose.tx = cfg.getFloat("sensor.tx", 0.0f);
    pose.ty = cfg.getFloat("sensor.ty", 0.0f);
    pose.tz = cfg.getFloat("sensor.tz", 0.0f);
    pose.roll_deg  = cfg.getFloat("sensor.roll_deg", 0.0f);
    pose.pitch_deg = cfg.getFloat("sensor.pitch_deg", 0.0f);
    pose.yaw_deg   = cfg.getFloat("sensor.yaw_deg", 0.0f);
    return pose;
}

// 생성자: 센서 좌표를 엘리베이터 좌표로 바꿔주는 변환기로 내부에 회전행렬 R_과 이동 벡터 t_을 가지고 있음.
// pose에 담긴 각도, 위치를 통해 R_, t_를 계산하여 저장
// doppler, power, target_id, zone는 변환하지 않음. (회전/이동은 위치(x,y,z)만 바꾸므로)
CoordinateTransform::CoordinateTransform(const SensorPose& pose){
    // 세 가지 회전 각도를 라디안으로 변환하여 (r, p, y)로 저장
    const float r = pose.roll_deg * kDegToRad;
    const float p = pose.pitch_deg * kDegToRad;
    const float y = pose.yaw_deg * kDegToRad;
    // cr, sr, cp, sp, cy, sy에 각각 cos/sin 값 계산
    const float cr = std::cos(r);
    const float sr = std::sin(r);
    const float cp = std::cos(p);
    const float sp = std::sin(p);
    const float cy = std::cos(y);
    const float sy = std::sin(y);

    // 회전 행렬을 apply가 아닌 생성자에서 계산하는 이유: 
    // apply는 프레임 단위로 수천~수만 번 호출되므로, 매번 회전행렬을 계산하면 느려짐. 
    // 따라서 생성자에서 한 번만 계산하고 저장해두고 apply에서는 저장된 R_을 사용.

    // R = Rz(yaw) * Ry(pitch) *Rx(roll)을 전개한 결과
    R_[0][0] = cy*cp; R_[0][1] = cy*sp*sr - sy*cr; R_[0][2] = cy*sp*cr + sy*sr;
    R_[1][0] = sy*cp; R_[1][1] = sy*sp*sr + cy*cr; R_[1][2] = sy*sp*cr - cy*sr;
    R_[2][0] = -sp;   R_[2][1] = cp*sr;            R_[2][2] = cp*cr;

    // TODO 3: t_ 에 tx, ty, tz 저장
    t_[0] = pose.tx;
    t_[1] = pose.ty;
    t_[2] = pose.tz;
}

void CoordinateTransform::apply(RadarPoint& pt) const{
    // pt.x, pt.y, pt.z를 R *(x,y,z)+t로 변환
     //   예) 새 x = R_[0][0]*x + R_[0][1]*y + R_[0][2]*z + t_[0]
    
     // pt.x, y, z를 먼저 덮어쓰면 다음 줄 계산이 틀어지기 때문에 복사해둠.
    const float x = pt.x;
    const float y = pt.y;
    const float z = pt.z;

    pt.x = R_[0][0]*x + R_[0][1]*y + R_[0][2]*z + t_[0];
    pt.y = R_[1][0]*x + R_[1][1]*y + R_[1][2]*z + t_[1];
    pt.z = R_[2][0]*x + R_[2][1]*y + R_[2][2]*z + t_[2];
}

void CoordinateTransform::apply(Frame& points) const{
    for(auto& p: points){
        apply(p);
    }
}