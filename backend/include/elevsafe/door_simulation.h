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
        enum class State { Closed, Opening, Open, Closing, Unknown };
        enum class Command { Open, Hold, Close };

        static constexpr auto openDuration = std::chrono::milliseconds(2680);
        static constexpr auto closeDuration = std::chrono::milliseconds(2350);
        static constexpr auto openHoldDuration = std::chrono::milliseconds(4000);
        // Initial software state only; no B observation or hardware input is inferred.
        explicit DoorSimulation(State initial = State::Closed);
        // Returns false for an identical active request; its deadline is preserved.
        bool apply(Command command, Time now);
        // Local arbitration seam, not a C interface. No input is a no-op, not release.
        // Auto Open/Hold persist until replaced or overridden by a manual request.
        // Inputs rejected during Manual are not queued for later execution.
        bool applyAuto(std::optional<Command> command, Time now);
        // Finish movement before AUTO selection, without dispatching idle close.
        bool advanceMotion(Time now) { return completeMotion(now); }
        // Explicit software risk-clear. Never releases Manual or emits an ACK.
        bool releaseAutoHold(Time now);
        bool update(Time now);
        // Latest applied command since last consumption, never a completion ACK.
        std::optional<Command> takeAppliedCommand() noexcept;
        std::optional<Time> openHoldDeadline() const noexcept { return openHoldDeadline_; }
        Mode mode() const noexcept { return mode_; }
        State state() const noexcept { return state_; }
        std::optional<Command> activeCommand() const noexcept { return active_; }
        std::optional<Time> startedAt() const noexcept { return started_; }
        std::optional<Time> deadline() const noexcept { return deadline_; }
        static std::string_view name(State state) noexcept;
        static std::string_view name(Command command) noexcept;

    private:
        bool completeMotion(Time now);
        void start(Command command, Mode source, Time now);
        Mode mode_ = Mode::Auto;
        State state_ = State::Closed;
        std::optional<Command> active_;
        std::optional<Time> started_, deadline_, openHoldDeadline_, lastManualAt_;
        std::optional<Command> applied_;
    };
}
