// roi_filter.h
// #1 ROI 설정: 출입구(Door Zone) 및 내부(Cabin) 좌표 범위

// ROI(Range-of-Interest): 레이더가 인식하는 전체 공간 중 실제로 우리가 주목해야 할 유효한 공간
//                         (출입구 앞 Door Zone, 내부 Cabin 공간)만 남기고 나머지는 버리는 영역 설정.
#pragma once

#include <vector>
#include <cmath>

using namespace std;

// 4D 레이터 포인트 클라우드 구조체
struct RadarPoint {
    float x;
    float y;
    float z;
    float doppler;
    float power;
    int target_id;
};

class ROIFilter {
private:
    // 엘레베이터 규격에 따른 ROI 범위 설정 (단위: 미터)
    float x_min_, x_max_; // 좌우 폭 범위
    float y_min_, y_max_; // 전방 거리 범위
    float z_min_, z_max_; // 높이 범위
    float min_power_;     // 최소 신호 세기(Power) 임계값(허공 노이즈 제거 용도)
    float doppler_threshold_; // 정지 상태로 간주할 도플러 속도 임계값 (ex. 0.05 m/s)

public:
    // 생성자에서 초기 ROI 범위 및 파라미터 설정
    ROIFilter(float x_min = -1.0f, float x_max = 1.0f,
              float y_min =  0.0f, float y_max = 3.0f,
              float z_min = -0.5f, float z_max = 2.5f,
              float min_power = 10.0f,
              float doppler_threshold = 0.05f)
            : x_min_(x_min), x_max_(x_max),
              y_min_(y_min), y_max_(y_max),
              z_min_(z_min), z_max_(z_max),
              min_power_(min_power),
              doppler_threshold_(doppler_threshold) {}

    // 필터링 실행 함수(ROI + Power + 정적 배경 제거)
    vector<RadarPoint> filter(const vector<RadarPoint>& raw_points){
        vector<RadarPoint> filtered_points;
        filtered_points.reserve(raw_points.size()); // 메모리 할당 최적화

        for(const auto& p: raw_points){
            // 1. 공간 좌표 범위 검사(x, y, z)
            bool in_x = (p.x >= x_min_ && p.x <= x_max_);
            bool in_y = (p.y >= y_min_ && p.y <= y_max_);
            bool in_z = (p.z >= z_min_ && p.z <= z_max_);

            // 2. 신호 세기(Power) 임계값 검사 (허공 노이즈 제거)
            bool valid_power = (p.power >= min_power_);

            // 3. 정적 배경 제거 검사
            bool is_not_background = removeStaticBackground(p);

            // 모든 조건 만족하는 점들만 생존
            if(in_x && in_y && in_z && valid_power && is_not_background){
                filtered_points.push_back(p);
            }
        }
        return filtered_points;
    }

private:
    // 정적 배경 제거 로직
    bool removeStaticBackground(const RadarPoint& p){
        // 만일 도플러 속도가 0에 매우 가깝다면
        if(abs(p.doppler) < doppler_threshold_){
            // 엘레베이터 x축 최외곽 5cm 부근에 고정된 점들은 벽으로 간주하고 제거. 
            if(p.x <= (x_min_ + 0.05f) || p.x >= (x_max_ - 0.05f)){
                return false;
            }
        }
        return true; // 움직이는 객체이거나 내부 유효 포인트이므로 유지.
    }

public:
    // 필요시 파라미터 동적 변경 메소드
    // 엘레베이터마다 물리적 규격이 다르니, 그에 맞게 동적으로 바꾸기 위함. 
    void setROI(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max){
        x_min_ = x_min; x_max_ = x_max;
        y_min_ = y_min; y_max_ = y_max;
        z_min_ = z_min; z_max_ = z_max; 
    }
};