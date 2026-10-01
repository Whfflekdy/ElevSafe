#include "preprocessing/roi_filter.h"

ROIFilter::ROIFilter(const ROIConfig& config): config_(config){}

Zone ROIFilter::classify(const RadarPoint& p) const{
    // door_zone 안이면 DOOR, 아니고 cabin_zone 안이면 CABIN, 아니면 NONE
    if(config_.door_zone.contains(p)){
        return Zone::DOOR;
    }
    else if(config_.cabin_zone.contains(p)){
        return Zone::CABIN;
    }
    else return Zone::NONE;
}

Frame ROIFilter::filter(const Frame& points, ROIStats* stats) const{
    Frame out;
    out.reserve(points.size()); // 미리 메모리 할당
    
    ROIStats local;
    local.input = points.size();

    for(const auto& p: points){
        // classify로 영역 판별
        Zone zone = classify(p);

        // NONE이면 dropped 증가 후 continue
        if(zone == Zone::NONE){
            local.dropped++;
            continue;
        }
        // p를 복사해서 zone을 채우고 out에 push_back
        // 복사본을 만드는 이유: const RadarPoint& p는 읽기 전용이므로 zone을 채우려면 복사본이 필요함.
        RadarPoint p_out = p;
        p_out.zone = zone;
        out.push_back(p_out);

        // DOOR / CABIN count 증가
        if(zone == Zone::DOOR) local.door++;
        else if(zone == Zone::CABIN) local.cabin++;
    }
    // 통계를 받지 않는 호출(stats == nullptr)도 있으므로, stats가 nullptr이 아니면 local을 복사해서 전달
    if(stats) *stats = local;
    return out;
}