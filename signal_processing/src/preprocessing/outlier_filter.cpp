#include "preprocessing/outlier_filter.h"

#include <cmath>
#include <utility>

namespace elevsafe::sp{

OutlierConfig OutlierConfig::fromConfig(const Config& cfg){
    OutlierConfig c;
    c.rcs_enabled = cfg.getFloat("outlier.rcs_enabled", 0.0f)!=0.0f;
    c.rcs_min_db = cfg.getFloat("outlier.rcs_min_db", c.rcs_min_db);
    c.sor_enabled = cfg.getFloat("outlier.sor_enabled", 0.0f) != 0.0f;
    c.sor_radius = cfg.getFloat("outlier.sor_radius", c.sor_radius);
    c.sor_min_neighbors = static_cast<int>(
        cfg.getFloat("outlier.sor_min_neighbors", static_cast<float>(c.sor_min_neighbors)));

    // 센서 위치는 좌표 변환과 같은 키를 읽어 값이 어긋나지 않게 한다
    c.sensor_x = cfg.getFloat("sensor.tx", 0.0f);
    c.sensor_y = cfg.getFloat("sensor.ty", 0.0f);
    c.sensor_z = cfg.getFloat("sensor.tz", 0.0f);
    return c;
}
OutlierFilter::OutlierFilter(const OutlierConfig& config): config_(config) {}

// RCS 계산: power_stats.py의 rcs_db와 동일
// 레이더의 세기는 거리의 4제곱에 비례해 약해지므로 40 log10(거리)를 더해 보정함.
// 값이 0 이하이면 log10을 계산할 수 없어 가장 약한 값으로 처리
float OutlierFilter::rcsDb(const RadarPoint& p)const{
    const float dx = p.x - config_.sensor_x;
    const float dy = p.y - config_.sensor_y;
    const float dz = p.z - config_.sensor_z;
    const float range = std::sqrt(dx * dx + dy*dy + dz*dz);
    if(range <= 0.0f || p.power <= 0.0f) return -1000.0f;
    return 10.0f*std::log10(p.power) + 40.0f *std::log10(range);
}

// filter 순서: RCS를 먼저 수행해 약한 점을 지운 뒤에, 남은 점끼리 이웃을 센다.
// => 약한 점에 기대어 살아남는 외톨이를 잡을 수 있음. 
Frame OutlierFilter::filter(const Frame& input, OutlierStats* stats) const{
    OutlierStats local;
    local.input = input.size();

    // 1. RCS: 너무 약한 점 제거
    Frame strong;
    strong.reserve(input.size());
    for(const RadarPoint& p: input){
        if(config_.rcs_enabled && rcsDb(p) < config_.rcs_min_db) local.rcs_removed++;
        else strong.push_back(p);
    }
    // 2. SOR: 주변 이웃이 적은 점 제거
    Frame kept;
    if(!config_.sor_enabled){
        kept = std::move(strong);
    } else{
        kept.reserve(strong.size());
        const float r2 = config_.sor_radius * config_.sor_radius;
        for(std::size_t i=0; i<strong.size(); ++i){
            int neighbors = 0;
            // 필요한 이웃 수를 채우면 더 볼 필요가 없으므로 멈춘다.
            for(std::size_t j=0; j<strong.size() && neighbors < config_.sor_min_neighbors; ++j){
                if (i == j) continue;
                const float dx = strong[i].x - strong[j].x;
                const float dy = strong[i].y - strong[j].y;
                const float dz = strong[i].z - strong[j].z;
                if (dx * dx + dy * dy + dz * dz <= r2) neighbors++;
            }
            if (neighbors >= config_.sor_min_neighbors) kept.push_back(strong[i]);
            else local.sor_removed++;
        }
    }

    if(stats != nullptr) *stats = local;
    return kept;
}
// 기존 SOR은 이웃과의 평균 거리를 계산하지만 여기서는 반경 안의 이웃 수만 셈
// 뜻이 쉽고 멈출 수도 있어서 더 빠르기 때문. 그렇지만 이름은 SOR로 유지. 

} // namespace elevsafe::sp