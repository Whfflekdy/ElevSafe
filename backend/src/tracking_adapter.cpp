#include "elevsafe/tracking_adapter.h"
#include "elevsafe/websocket_message.h"

#include <nlohmann/json.hpp>
#include <ctime>
#include <stdexcept>

namespace elevsafe
{
    FrameDetections toTrackingDetections(const sp::PipelineResult& result)
    {
        if (result.timestamp_us < 0) throw std::invalid_argument("negative B timestamp_us");
        FrameDetections frame;
        frame.frame = result.frame_id;
        frame.timestamp_us = result.timestamp_us;
        frame.detections.reserve(result.clusters.size());
        for (const auto& cluster : result.clusters)
        {
            if (cluster.num_points < 40) continue; // Demo-only threshold; B/C stay unchanged.
            Detection detection;
            detection.cluster = cluster.cluster_id;
            detection.cx = cluster.cx; detection.cy = cluster.cy; detection.cz = cluster.cz;
            detection.sx = cluster.x_max - cluster.x_min;
            detection.sy = cluster.y_max - cluster.y_min;
            detection.sz = cluster.z_max - cluster.z_min;
            detection.zmin = cluster.z_min; detection.zmax = cluster.z_max;
            detection.n_points = cluster.num_points;
            detection.mean_doppler = cluster.mean_doppler;
            detection.mean_power = 0; // Unavailable in B, NOT a measured zero. C currently ignores it.
            frame.detections.push_back(detection);
        }
        return frame;
    }

    std::string trackingSnapshot(const sp::PipelineResult& result,
        const std::vector<TrackOutput>& tracks, std::chrono::system_clock::time_point publishedAt)
    {
        const auto seconds = std::chrono::system_clock::to_time_t(publishedAt);
        std::tm utc{};
#ifdef _WIN32
        if (gmtime_s(&utc, &seconds) != 0) throw std::runtime_error("UTC conversion failed");
#else
        if (!gmtime_r(&seconds, &utc)) throw std::runtime_error("UTC conversion failed");
#endif
        char timestamp[32]{};
        if (!std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc))
            throw std::runtime_error("UTC formatting failed");
        auto passengers = nlohmann::json::array();
        for (const auto& track : tracks)
        {
            if (track.state != TrackState::CONFIRMED) continue;
            passengers.push_back({{"id", track.id}, {"x", track.x}, {"y", track.y},
                {"state", track.y < 0 ? "OUTSIDE" : "INSIDE"}, {"safety_state", nullptr}});
        }
        const auto text = nlohmann::json({{"timestamp", timestamp},
            {"timestamp_us", result.timestamp_us}, {"door_state", sp::doorStateToString(result.estimated_door_state)},
            {"door_command", nullptr}, {"passengers", passengers}}).dump();
        if (!isSnapshot(text)) throw std::runtime_error("invalid tracking snapshot");
        return text;
    }

    std::optional<std::string> TrackingDemo::process(const sp::PipelineResult& result,
        Clock::time_point now, std::chrono::system_clock::time_point publishedAt)
    {
        const auto tracks = engine_.processDetections(toTrackingDetections(result), true);
        ++processedFrames_;
        if (lastPublish_ && now - *lastPublish_ < publishPeriod) return std::nullopt;
        const auto snapshot = trackingSnapshot(result, tracks, publishedAt);
        lastPublish_ = now;
        return snapshot; // No timer emits old data when Radar input stops.
    }
}
