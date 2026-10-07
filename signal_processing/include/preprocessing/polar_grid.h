// polar_grid.h
// [Step 2] 센서 기준 극좌표 격자 번호 계산
//
// Retina-4SN 의 점은 다음 격자 위에만 찍힌다 (미니테스트 데이터로 확인):
//   거리 r = sqrt(x^2 + y^2 + z^2)      간격 0.071565 m
//   좌우 u = x / r  (센서 오른쪽 +X)     간격 0.015938
//   위아래 v = z / r (센서 위쪽 +Z)      간격 0.015938
// 세 값 모두 0 을 기준으로 정렬되어 있어 round(값 / 간격) 으로 칸 번호를 구한다.
//
// 반드시 좌표 변환 "전"(센서 좌표)에 호출해야 한다.
#pragma once

#include "common/config.h"
#include "common/radar_point.h"

struct PolarGridConfig {
    float range_res = 0.071565f;
    float uv_res = 0.015938f;

    static PolarGridConfig fromConfig(const Config& cfg);
};

class PolarGrid {
public:
    explicit PolarGrid(const PolarGridConfig& config);

    void assignBins(RadarPoint& p) const;
    void assignBins(Frame& points) const;

    const PolarGridConfig& config() const { return config_; }

private:
    PolarGridConfig config_;
};