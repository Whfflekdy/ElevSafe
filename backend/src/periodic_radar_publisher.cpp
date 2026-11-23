#include "elevsafe/periodic_radar_publisher.h"
#include "elevsafe/websocket_server.h"

#include <stdexcept>
#include <utility>

namespace elevsafe
{
    void RadarSnapshotSchedule::observe(std::int64_t timestampUs, std::vector<TrackOutput> tracks,
        std::string_view doorState, std::optional<std::string_view> applied, Time now)
    {
        timestampUs_ = timestampUs;
        tracks_ = std::move(tracks);
        updateDoor(doorState, applied);
        if (!due_) due_ = now;
    }

    void RadarSnapshotSchedule::updateDoor(std::string_view doorState, std::optional<std::string_view> applied)
    {
        doorState_ = doorState;
        if (applied) applied_ = std::string(*applied);
    }

    std::optional<std::string> RadarSnapshotSchedule::poll(Time now, std::chrono::system_clock::time_point publishedAt)
    {
        if (!due_ || now < *due_) return std::nullopt;
        auto text = trackingSnapshot(timestampUs_, tracks_, publishedAt, doorState_,
            applied_ ? std::optional<std::string_view>(*applied_) : std::nullopt);
        applied_.reset();
        due_ = now + TrackingDemo::publishPeriod;
        return text;
    }

    void RadarSnapshotSchedule::end()
    {
        due_.reset();
        applied_.reset();
        tracks_.clear();
    }

    PeriodicRadarPublisher::PeriodicRadarPublisher(WebSocketServer& server)
        : server_(server), worker_([this] { run(); }) {}

    PeriodicRadarPublisher::~PeriodicRadarPublisher()
    {
        stop();
    }

    void PeriodicRadarPublisher::observe(std::int64_t timestampUs, std::vector<TrackOutput> tracks,
        std::string_view doorState, std::optional<std::string_view> applied)
    {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        const bool first = !schedule_.due();
        schedule_.observe(timestampUs, std::move(tracks), doorState, applied, RadarSnapshotSchedule::Clock::now());
        if (first) changed_.notify_one();
    }

    void PeriodicRadarPublisher::updateDoor(std::string_view doorState, std::optional<std::string_view> applied)
    {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        schedule_.updateDoor(doorState, applied);
    }

    void PeriodicRadarPublisher::run()
    {
        using Clock = RadarSnapshotSchedule::Clock;
        std::unique_lock lock(mutex_);
        try
        {
            while (!stopping_)
            {
                if (const auto due = schedule_.due())
                {
                    if (changed_.wait_until(lock, *due, [this] { return stopping_; })) break;
                    if (auto snapshot = schedule_.poll(Clock::now(), std::chrono::system_clock::now()))
                    {
                        // Serialize and enqueue under the same lock as stop. No enqueue can start
                        // after the Radar owner invalidates this session.
                        const auto at = Clock::now();
                        if (!server_.publishSnapshot(std::move(*snapshot)))
                            throw std::runtime_error("periodic tracking snapshot publish failed");
                        if (published_++ == 0) first_ = at;
                        last_ = at;
                    }
                }
                else
                    changed_.wait(lock, [this] { return stopping_ || schedule_.due().has_value(); });
            }
        }
        catch (...)
        {
            failure_ = std::current_exception();
        }
    }

    void PeriodicRadarPublisher::stop()
    {
        if (!worker_.joinable()) return;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            schedule_.end();
        }
        changed_.notify_one();
        worker_.join();
    }

    PeriodicRadarPublisher::Statistics PeriodicRadarPublisher::finish()
    {
        stop();
        if (failure_) std::rethrow_exception(failure_);
        const auto spanUs = std::chrono::duration_cast<std::chrono::microseconds>(last_ - first_).count();
        return {published_, published_ > 1 ? spanUs / static_cast<std::int64_t>(published_ - 1) : 0};
    }
}
