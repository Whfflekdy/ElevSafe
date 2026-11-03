// outlier_filter.h
// [Step 3] 허공 노이즈 제거. 둘 다 기본 꺼짐, 켜는 값은 승객 장면 녹화로 정한다.
//   RCS: 거리 효과를 보정한 세기가 너무 약한 점 제거
//   SOR: 주변 이웃이 너무 적은 외톨이 점 제거
#pragma once

#include <cstddef>

#include "common/config.h"
#include "common/radar_point.h"

namespace elevsafe::sp {

struct OutlierConfig {
    bool rcs_enabled = false;
    float rcs_min_db = 30.0f;    // 이 값보다 약하면 제거 (상대값)
    bool sor_enabled = false;
    float sor_radius = 0.30f;    // 이웃을 찾을 거리 [m]
    int sor_min_neighbors = 3;   // 이보다 이웃이 적으면 제거 (자기 자신 제외)
    float sensor_x = 0.0f, sensor_y = 0.0f, sensor_z = 0.0f;  // 공통 좌표의 센서 위치

    static OutlierConfig fromConfig(const Config& cfg);
};

struct OutlierStats {
    std::size_t input = 0;
    std::size_t rcs_removed = 0;
    std::size_t sor_removed = 0;
};

class OutlierFilter {
public:
    explicit OutlierFilter(const OutlierConfig& config);
    Frame filter(const Frame& input, OutlierStats* stats = nullptr) const;
    float rcsDb(const RadarPoint& p) const;   // 거리 보정한 세기
private:
    OutlierConfig config_;
};

}  // namespace elevsafe::sp