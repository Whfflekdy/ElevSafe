#pragma once

#include "pipeline/signal_pipeline.h"
#include "tracking_api.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace elevsafe
{
    // Presentation-only mapping. No second clustering or C state/safety judgment.
    FrameDetections toTrackingDetections(const sp::PipelineResult& result);
    std::string trackingSnapshot(const sp::PipelineResult& result,
        const std::vector<TrackOutput>& tracks, std::chrono::system_clock::time_point publishedAt);

    // One instance per Radar TCP session. Feed EVERY B result, including empty frames.
    class TrackingDemo
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto publishPeriod = std::chrono::milliseconds(100);
        std::optional<std::string> process(const sp::PipelineResult& result,
            Clock::time_point now, std::chrono::system_clock::time_point publishedAt);
        std::size_t processedFrames() const { return processedFrames_; }
    private:
        TrackingEngine engine_;
        std::optional<Clock::time_point> lastPublish_;
        std::size_t processedFrames_ = 0;
    };
}
