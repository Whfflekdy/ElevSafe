#include "preprocessing/polar_grid.h"

#include <cmath>

PolarGridConfig PolarGridConfig::fromConfig(const Config& cfg) {
    PolarGridConfig c;
    // TODO: cfg 에서 "polar.range_res", "polar.uv_res" 읽기 (기본값은 c 의 현재 값)
    c.range_res = cfg.getFloat("polar.range_res", c.range_res);
    c.uv_res = cfg.getFloat("polar.uv_res", c.uv_res);
    return c;
}

PolarGrid::PolarGrid(const PolarGridConfig& config) : config_(config) {}

void PolarGrid::assignBins(RadarPoint& p) const {
    // TODO 1: 거리 r = sqrt(x^2 + y^2 + z^2) 계산 (std::sqrt)
    const float r = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
    // TODO 2: r 이 너무 작으면 (예: 1e-6 미만) 방향을 정할 수 없으니
    //         has_bins = false 로 두고 return
    if(r<1e-6){
        p.has_bins = false;
        return;
    }
    // TODO 3: range_bin = round(r / range_res)
    //         u_bin     = round((x / r) / uv_res)
    //         v_bin     = round((z / r) / uv_res)
    //         (std::lround 를 쓰면 반올림한 정수를 바로 돌려준다)
    p.range_bin = static_cast<int>(std::lround(r / config_.range_res));
    p.u_bin = static_cast<int>(std::lround((p.x/r)/config_.uv_res));
    p.v_bin = static_cast<int>(std::lround((p.z/r)/config_.uv_res));
    // TODO 4: has_bins = true
    p.has_bins = true;
}

void PolarGrid::assignBins(Frame& points) const {
    for (auto& p : points) assignBins(p);
}