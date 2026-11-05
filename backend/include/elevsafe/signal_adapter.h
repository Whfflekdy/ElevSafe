#pragma once

#include "radar_types.h"
#include "common/radar_point.h"

namespace elevsafe
{
    // Throws std::overflow_error if a source integer cannot fit in B's type.
    sp::FrameData toSignalFrame(const RadarFrame& frame);
}
