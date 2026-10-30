#include "elevsafe/signal_adapter.h"

#include <stdexcept>
#include <utility>

namespace elevsafe
{
    sp::FrameData toSignalFrame(const RadarFrame& frame)
    {
        sp::FrameData result;
        if (!std::in_range<decltype(result.frame_id)>(frame.frameCount))
            throw std::overflow_error("frameCount exceeds B frame_id range");
        if (!std::in_range<decltype(result.timestamp_us)>(frame.timestampUs))
            throw std::overflow_error("timestampUs exceeds B timestamp_us range");

        result.frame_id = static_cast<decltype(result.frame_id)>(frame.frameCount);
        result.timestamp_us = static_cast<decltype(result.timestamp_us)>(frame.timestampUs);
        result.points.reserve(frame.points.size());
        for (const auto& point : frame.points)
        {
            sp::RadarPoint converted;
            if (!std::in_range<decltype(converted.target_id)>(point.targetId))
                throw std::overflow_error("targetId exceeds B target_id range");
            converted.x = point.x;
            converted.y = point.y;
            converted.z = point.z;
            converted.doppler = point.doppler;
            converted.power = point.power;
            converted.target_id = static_cast<decltype(converted.target_id)>(point.targetId);
            result.points.push_back(converted);
        }
        return result;
    }
}
