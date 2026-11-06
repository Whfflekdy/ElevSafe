#include "elevsafe/door_simulation.h"

namespace elevsafe
{
    bool DoorSimulation::update(Time now)
    {
        if (!deadline_ || now < *deadline_) return false;
        state_ = state_ == State::Opening ? State::Open : State::Closed;
        started_.reset();
        deadline_.reset();
        if (active_ != Command::Hold)
        {
            active_.reset();
            mode_ = Mode::Auto;
        }
        return true;
    }

    bool DoorSimulation::apply(Command command, Time now)
    {
        update(now);
        if (active_ == command) return false;
        active_ = command;
        mode_ = Mode::Manual;
        const bool opening = command != Command::Close;
        if (state_ == (opening ? State::Open : State::Closed))
        {
            started_.reset();
            deadline_.reset();
            if (command != Command::Hold)
            {
                active_.reset();
                mode_ = Mode::Auto;
            }
        }
        else
        {
            // A different command replaces the old transition using its full duration.
            state_ = opening ? State::Opening : State::Closing;
            started_ = now;
            deadline_ = now + (opening ? openDuration : closeDuration);
        }
        return true;
    }

    std::string_view DoorSimulation::name(State state) noexcept
    {
        switch (state)
        {
        case State::Closed: return "CLOSED";
        case State::Opening: return "OPENING";
        case State::Open: return "OPEN";
        case State::Closing: return "CLOSING";
        }
        return "CLOSED";
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
