#include "preprocessing/roi_filter.h"
#include <iostream>
#include <string>

namespace{
// 설정 파일에서 박스 하나(6개 값)을 읽어 Box3D로 변환하는 함수
Box3D readBox(const Config& cfg, const std::string& prefix, const Box3D& def){
    Box3D b;
    // 문자열을 이어 붙여서 key(ex. "door.x_min")를 만들고,
    // 설정 파일에서 해당 key를 찾아 값을 꺼냄. 없으면 def에서 가져와 Box3D에 채움.
    b.x_min = cfg.getFloat(prefix+".x_min", def.x_min);
    b.x_max = cfg.getFloat(prefix+".x_max", def.x_max);
    b.y_min = cfg.getFloat(prefix+".y_min", def.y_min);
    b.y_max = cfg.getFloat(prefix+".y_max", def.y_max);
    b.z_min = cfg.getFloat(prefix+".z_min", def.z_min);
    b.z_max = cfg.getFloat(prefix+".z_max", def.z_max);
    return b;
}
}

ROIConfig ROIConfig::fromConfig(const Config& cfg){
    // 설정 파일에 값이 없을 때 쓸 기본값
    const Box3D default_door {-0.45f, 0.45f, -0.50f, 0.30f, 0.10f, 2.10f};
    const Box3D default_cabin{-0.75f, 0.75f,  0.30f, 1.35f, 0.10f, 2.20f};

    ROIConfig c;
    // c.door_zone, c.cabin_zone을 readBox로 읽기
    c.door_zone = readBox(cfg, "door", default_door);
    c.cabin_zone = readBox(cfg, "cabin", default_cabin);

    if(!c.isValid()){
        std::cerr << "[ROIConfig] 잘못된 ROI 범위 (min >= max). 설정 파일을 확인하세요. \n";
    }
    return c;
}

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