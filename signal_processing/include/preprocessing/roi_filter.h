// roi_filter.h
// #1 ROI 필터: 출입구(Door Zone) 및 내부(Cabin) 좌표 범위 안의 포인트만 남기고 Zone 라벨을 붙임.
// 이 클래스는 공간 범위만 담당
//   - Power/RCS 허공 노이즈 제거 -> outlier_filter (Step 3)
//   - 벽/문 정적 배경 제거        -> background_subtractor (Step 2)
// ROI(Range-of-Interest): 레이더가 인식하는 전체 공간 중 실제로 우리가 주목해야 할 유효한 공간
//                         (출입구 앞 Door Zone, 내부 Cabin 공간)만 남기고 나머지는 버리는 영역 설정.
#pragma once

#include <vector>
#include <cmath>
#include <cstddef>

#include "common/radar_point.h"
#include "common/box3d.h"
#include "common/config.h"

// ROI 설정: 두 영역
struct ROIConfig{
    Box3D door_zone;  // 출입구 영역
    Box3D cabin_zone; // 엘리베이터 내부 영역

    static ROIConfig fromConfig(const Config& cfg);
    bool isValid() const{
        return door_zone.isValid() && cabin_zone.isValid();
    }
};

// 필터링 통계 (단계별로 몇 개 남았는지 확인하기 위함)
struct ROIStats{
    std::size_t input = 0;  // 필터에 들어온 포인트 수
    std::size_t door = 0;   // Door Zone으로 분류되어 남은 포인트 수
    std::size_t cabin = 0;  // Cabin Zone으로 분류되어 남은 포인트 수
    std::size_t dropped = 0; // 필터링되어 제거된 포인트 수
};

class ROIFilter {
public: 
    // 생성자 앞의 explicit: 단일 인자 생성자에 대해 암시적 형변환을 막음. (ROIConfig를 ROIFilter로 변환하는 것을 막음)
    explicit ROIFilter(const ROIConfig& config);

    // 점 하나가 어느 영역에 속하는지 판별하고 겹치는 경계는 Door Zone 우선으로 라벨링
    Zone classify(const RadarPoint& p) const;

    // ROI 안의 포인트만 남기고 zone을 채워 반환. stats가 nullptr이 아니면 통계 기록(통계는 선택사항으로 두기 위함)
    Frame filter(const Frame& input, ROIStats* stats=nullptr) const;

    void setConfig(const ROIConfig& config){config_ = config;}
    const ROIConfig& config() const {return config_;}
private:
    ROIConfig config_;
};