#pragma once

#include "elevsafe/tracking_adapter.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace elevsafe
{
    class WebSocketServer;

    // Session-local state; time is injected for deterministic cadence and teardown tests.
    class RadarSnapshotSchedule
    {
    public:
        using Clock = TrackingDemo::Clock;
        using Time = Clock::time_point;

        void observe(std::int64_t timestampUs, std::vector<TrackOutput> tracks,
            std::string_view doorState, std::optional<std::string_view> applied, Time now);
        void updateDoor(std::string_view doorState, std::optional<std::string_view> applied);
        std::optional<Time> due() const { return due_; }
        std::optional<std::string> poll(Time now, std::chrono::system_clock::time_point publishedAt);
        void end();

    private:
        std::optional<Time> due_;
        std::int64_t timestampUs_ = 0;
        std::vector<TrackOutput> tracks_;
        std::string doorState_;
        std::optional<std::string> applied_;
    };

    // The Radar thread remains the only owner of B, C and the door simulation.
    class PeriodicRadarPublisher
    {
    public:
        struct Statistics
        {
            std::size_t published = 0;
            std::int64_t meanIntervalUs = 0;
        };

        explicit PeriodicRadarPublisher(WebSocketServer& server);
        ~PeriodicRadarPublisher();
        void observe(std::int64_t timestampUs, std::vector<TrackOutput> tracks,
            std::string_view doorState, std::optional<std::string_view> applied = std::nullopt);
        void updateDoor(std::string_view doorState, std::optional<std::string_view> applied);
        void checkFailure();
        // Linearize session invalidation with enqueue. Does not drain WebSocket queues.
        std::size_t seal();
        Statistics finish();

    private:
        void run();
        void stop();

        WebSocketServer& server_;
        std::mutex mutex_;
        std::condition_variable changed_;
        RadarSnapshotSchedule schedule_;
        bool stopping_ = false;
        std::exception_ptr failure_;
        std::size_t published_ = 0;
        RadarSnapshotSchedule::Time first_{}, last_{};
        // Start only after the shared state above has been initialized.
        std::thread worker_;
    };
}
