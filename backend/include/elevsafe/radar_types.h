#pragma once

#include <cstdint>
#include <vector>

namespace elevsafe
{
    struct RadarPoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float doppler = 0.0f;
        float power = 0.0f;
        std::int32_t targetId = -1;
    };

    struct RadarFrame
    {
        std::uint32_t frameCount = 0;
        std::vector<RadarPoint> points;
    };
}
