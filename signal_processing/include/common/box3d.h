// box3d.h
// 축 정렬 3D 박스 (ROI 영역 정의, 이후 Bounding Box에 재사용)
#pragma once

#include "common/radar_point.h"

struct Box3D {
    float x_min=0.0f, x_max=0.0f; // 좌우 폭 범위
    float y_min=0.0f, y_max=0.0f; // 전방 거리 범위
    float z_min=0.0f, z_max=0.0f; // 높이 범위

    // 아래 함수들 뒤 const: 함수가 멤버 변수를 바꾸지 않음을 명시. (const 멤버 함수)
    //                    const Box3D&로 전달받은 박스에서도 contains() 호출 가능하도록. 

    // ROI 영역 내에 포인트가 존재하는지 검사
    bool contains(const RadarPoint& p) const {
        return (p.x >= x_min && p.x <= x_max) &&
               (p.y >= y_min && p.y <= y_max) &&
               (p.z >= z_min && p.z <= z_max);
    }

    // 설정 값이 뒤집혀 있지 않은지 (min < max) 검사
    bool isValid() const {
        return (x_min < x_max) && (y_min < y_max) && (z_min < z_max);
    }

    float width() const { return x_max - x_min; };
    float depth() const { return y_max - y_min; };
    float height() const { return z_max - z_min; };
};