#include "elevsafe/door_simulation.h"
#include <stdexcept>

namespace elevsafe
{
    DoorSimulation::DoorSimulation(State initial) : state_(initial)
    {
        if (initial != State::Closed && initial != State::Unknown)
            throw std::invalid_argument("initial simulation state must be CLOSED or UNKNOWN");
    }

    bool DoorSimulation::completeMotion(Time now)
    {
        if (!deadline_ || now < *deadline_) return false;
        const auto completedAt = *deadline_;
        state_ = state_ == State::Opening ? State::Open : State::Closed;
        started_.reset();
        deadline_.reset();
        if (active_ != Command::Hold && !(mode_ == Mode::Auto && active_ == Command::Open))
        {
            active_.reset();
            mode_ = Mode::Auto;
            if (state_ == State::Open) openHoldDeadline_ = completedAt + openHoldDuration;
        }
        return true;
    }

    void DoorSimulation::start(Command command, Mode source, Time now)
    {
        // Repeated OPEN at an already open, unheld door does not extend its timer.
        const auto previousHoldDeadline = openHoldDeadline_;
        active_ = command;
        mode_ = source;
        applied_ = command;
        openHoldDeadline_.reset();
        const bool opening = command != Command::Close;
        if (state_ == (opening ? State::Open : State::Closed))
        {
            started_.reset();
            deadline_.reset();
            if (command != Command::Hold && !(source == Mode::Auto && command == Command::Open))
            {
                active_.reset();
                mode_ = Mode::Auto;
                if (state_ == State::Open)
                    openHoldDeadline_ = previousHoldDeadline.value_or(now + openHoldDuration);
            }
        }
        else
        {
            state_ = opening ? State::Opening : State::Closing;
            started_ = now;
            deadline_ = now + (opening ? openDuration : closeDuration);
        }
    }

    bool DoorSimulation::apply(Command command, Time now)
    {
        // Complete motion, but never dispatch an expired AUTO timer before Manual.
        completeMotion(now);
        lastManualAt_ = now;
        if (mode_ == Mode::Manual && active_ == command) return false;
        start(command, Mode::Manual, now);
        return true;
    }

    bool DoorSimulation::applyAuto(std::optional<Command> command, Time now)
    {
        if (!command) return false; // Not risk-clear or input-loss information.
        completeMotion(now);
        if (mode_ == Mode::Manual || lastManualAt_ == now || state_ == State::Unknown) return false;
        if (active_ == command) return false;
        if (*command == Command::Close && state_ == State::Closed) return false;
        start(*command, Mode::Auto, now);
        return true;
    }

    bool DoorSimulation::releaseAutoHold(Time now)
    {
        completeMotion(now);
        if (mode_ != Mode::Auto || lastManualAt_ == now
            || (active_ != Command::Open && active_ != Command::Hold)) return false;
        active_.reset();
        // Preserve in-flight opening; completion starts the normal hold.
        if (state_ == State::Open) openHoldDeadline_ = now + openHoldDuration;
        return true;
    }

    bool DoorSimulation::update(Time now)
    {
        bool changed = completeMotion(now);
        if (mode_ == Mode::Auto && state_ == State::Open && !active_
            && openHoldDeadline_ && now >= *openHoldDeadline_ && lastManualAt_ != now)
        {
            const auto closeAt = *openHoldDeadline_;
            start(Command::Close, Mode::Auto, closeAt);
            changed = true;
            completeMotion(now); // A delayed tick may span both deadlines, never a periodic OPEN.
        }
        return changed;
    }

    std::optional<DoorSimulation::Command> DoorSimulation::takeAppliedCommand() noexcept
    {
        const auto value = applied_;
        applied_.reset();
        return value;
    }

    std::string_view DoorSimulation::name(State state) noexcept
    {
        switch (state)
        {
        case State::Closed: return "CLOSED";
        case State::Opening: return "OPENING";
        case State::Open: return "OPEN";
        case State::Closing: return "CLOSING";
        case State::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    std::string_view DoorSimulation::name(Command command) noexcept
    {
        switch (command)
        {
        case Command::Open: return "OPEN";
        case Command::Hold: return "HOLD";
        case Command::Close: return "CLOSE";
        }
        return "";
    }
}
