#pragma once

#include "elevsafe/door_simulation.h"
#include "state_machine.hpp"

#include <cstdint>
#include <optional>

namespace elevsafe
{
    // Local to one Emulator TCP session. Evaluate only the C result for this frame.
    class EmulatorDoorArbitration
    {
    public:
        using Door = DoorSimulation;

        // The optional arrival flag lives in the Radar owner loop, across TCP sessions.
        explicit EmulatorDoorArbitration(Door& door, bool* arrivalPending = nullptr)
            : door_(door), arrivalPending_(arrivalPending) {}

        bool applyManual(Door::Command command, Door::Time now)
        {
            if (arrivalPending_) *arrivalPending_ = false;
            normalCycle_ = false;
            return door_.apply(command, now);
        }

        void advanceMotion(Door::Time now)
        {
            if (door_.advanceMotion(now) && normalCycle_ && door_.state() == Door::State::Open)
                normalCloseAt_ = now + normalOpenHold;
        }

        DoorState trackingDoorState() const
        {
            switch (door_.state())
            {
            case Door::State::Closed: return DoorState::CLOSED;
            case Door::State::Open: return DoorState::OPEN;
            case Door::State::Closing:
                // Auto closing still admits a fresh threshold-occupancy HOLD.
                return door_.mode() == Door::Mode::Auto ? DoorState::OPEN : DoorState::UNKNOWN;
            case Door::State::Opening:
            case Door::State::Unknown: return DoorState::UNKNOWN;
            }
            return DoorState::UNKNOWN;
        }

        bool applyFrame(DoorCommand command, std::int64_t timestampUs, Door::Time receivedAt,
            Door::Time now, bool manualConsumed, DoorState evaluatedDoor = DoorState::UNKNOWN)
        {
            if (!recordFrame(timestampUs, receivedAt, now)) return false;
            if (manualConsumed || door_.mode() != Door::Mode::Auto) return false;

            // A motion may have completed while B/C was processing the frame.
            advanceMotion(now);
            const auto state = door_.state();
            if (command == DoorCommand::HOLD &&
                (state == Door::State::Open || state == Door::State::Closing))
            {
                if (!door_.applyAuto(Door::Command::Hold, now)) return false;
                normalCycle_ = false; // C takes over this cycle's close decision.
                return true;
            }

            if (command == DoorCommand::NONE && arrivalPending_ && *arrivalPending_ &&
                state == Door::State::Closed && door_.applyAuto(Door::Command::Open, now))
            {
                *arrivalPending_ = false;
                normalCycle_ = true;
                return true;
            }

            // NONE only distinguishes a normal timer candidate from a C command.
            const bool normalClose = command == DoorCommand::NONE && normalCycle_ &&
                normalCloseAt_ && now >= *normalCloseAt_ && evaluatedDoor == DoorState::OPEN;
            if ((command == DoorCommand::CLOSE || normalClose) && state == Door::State::Open &&
                continuousFrames_ >= minimumFreshFrames &&
                receivedAt - continuousSince_ >= minimumFreshSpan &&
                timestampUs - continuousSinceUs_ >= minimumFreshSpan.count() &&
                door_.applyAuto(Door::Command::Close, now))
            {
                normalCycle_ = false;
                return true;
            }
            return false;
        }

    private:
        // Emulator verification policy. These are independent of D's display timeout.
        static constexpr auto maximumGap = std::chrono::microseconds{1000000};
        static constexpr auto minimumFreshSpan = std::chrono::microseconds{1000000};
        static constexpr unsigned minimumFreshFrames = 3;
        // Provisional simulation dwell, based on the existing Mock hold time.
        static constexpr auto normalOpenHold = std::chrono::seconds{4};

        struct Observation
        {
            std::int64_t timestampUs;
            Door::Time receivedAt;
        };

        bool recordFrame(std::int64_t timestampUs, Door::Time receivedAt, Door::Time now)
        {
            if (now - receivedAt > maximumGap)
            {
                last_.reset();
                return false;
            }
            if (last_ && timestampUs <= last_->timestampUs) return false;

            if (!last_ || receivedAt - last_->receivedAt > maximumGap ||
                timestampUs - last_->timestampUs > maximumGap.count())
            {
                continuousSince_ = receivedAt;
                continuousSinceUs_ = timestampUs;
                continuousFrames_ = 1;
            }
            else if (continuousFrames_ < minimumFreshFrames)
            {
                ++continuousFrames_;
            }
            last_ = Observation{timestampUs, receivedAt};
            return true;
        }

        Door& door_;
        bool* arrivalPending_;
        bool normalCycle_ = false;
        std::optional<Door::Time> normalCloseAt_;
        std::optional<Observation> last_;
        Door::Time continuousSince_{};
        std::int64_t continuousSinceUs_ = 0;
        unsigned continuousFrames_ = 0;
    };
}
