#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace elevsafe
{
    // Software-only model. Never a physical door observation or actuator.
    class DoorSimulation
    {
    public:
        using Clock = std::chrono::steady_clock;
        using Time = Clock::time_point;
        enum class Mode { Auto, Manual };
        enum class State { Closed, Opening, Open, Closing };
        enum class Command { Open, Hold, Close };

        static constexpr auto openDuration = std::chrono::milliseconds(2680);
        static constexpr auto closeDuration = std::chrono::milliseconds(2350);
        // Returns false for an identical active request; its deadline is preserved.
        bool apply(Command command, Time now);
        bool update(Time now);
        Mode mode() const noexcept { return mode_; }
        State state() const noexcept { return state_; }
        std::optional<Command> activeCommand() const noexcept { return active_; }
        std::optional<Time> startedAt() const noexcept { return started_; }
        std::optional<Time> deadline() const noexcept { return deadline_; }
        static std::string_view name(State state) noexcept;
        static std::string_view name(Command command) noexcept;

    private:
        Mode mode_ = Mode::Auto;
        State state_ = State::Closed;
        std::optional<Command> active_;
        std::optional<Time> started_, deadline_;
    };
}
