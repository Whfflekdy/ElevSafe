#pragma once

#include "elevsafe/door_simulation.h"
#include "elevsafe/websocket_message.h"
#include <nlohmann/json.hpp>
#include <array>
#include <optional>
#include <string_view>
#include <stdexcept>

namespace elevsafe
{
    // Synthetic inputs only: NOT C tracking, a hardware interlock, or a C API.
    // The default Mock does not use this policy; an explicit scenario opts in.
    class MockDoorPolicy
    {
        nlohmann::json passengers_ = nlohmann::json::array();
        bool doorwayRisk_ = false, releasePending_ = false, inputMissing_ = false;
    public:
        // nullopt means missing supply; an empty array is an explicit clear.
        // Invalid input is rejected, not repaired/published. Keep last valid input.
        bool accept(const std::optional<nlohmann::json>& input)
        {
            if (!input) { inputMissing_ = true; return false; }
            auto frame = nlohmann::json::parse(mockSnapshot());
            frame["passengers"] = *input;
            if (!isSnapshot(frame.dump())) { inputMissing_ = true; return false; }
            bool risk = false;
            for (const auto& passenger : *input)
            {
                const auto& state = passenger["state"];
                const bool unsafe = !passenger["safety_state"].is_null();
                if (state == "OUTSIDE" && unsafe) { inputMissing_ = true; return false; }
                if ((state == "ENTERING" || state == "EXITING") && unsafe) risk = true;
            }
            releasePending_ = risk ? false : (releasePending_ || doorwayRisk_);
            doorwayRisk_ = risk;
            passengers_ = *input;
            inputMissing_ = false;
            return true;
        }
        // Caller drains manual FIFO first using the same time point.
        bool tick(DoorSimulation& door, DoorSimulation::Time now)
        {
            bool changed = door.advanceMotion(now);
            if (releasePending_)
            {
                changed = door.releaseAutoHold(now) || changed;
                releasePending_ = false;
            }
            // Missing supply preserves existing AUTO latch, but cannot issue new commands.
            if (doorwayRisk_ && !inputMissing_)
            {
                const auto command = door.state() == DoorSimulation::State::Open
                    ? DoorSimulation::Command::Hold : DoorSimulation::Command::Open;
                changed = door.applyAuto(command, now) || changed;
            }
            return door.update(now) || changed;
        }
        bool inputMissing() const noexcept { return inputMissing_; }
        bool doorwayRisk() const noexcept { return doorwayRisk_; }
        const nlohmann::json& passengers() const noexcept { return passengers_; }
    };

    inline constexpr std::array<std::string_view, 12> mockDoorScenarioNames{
        "normal", "unknown", "inside-fall", "inside-immobile", "entering-fall",
        "entering-immobile", "exiting-fall", "exiting-immobile", "risk-clear",
        "input-loss", "multi", "manual-conflict"
    };
    inline bool isMockDoorScenario(std::string_view name)
    {
        for (const auto candidate : mockDoorScenarioNames) if (candidate == name) return true;
        return false;
    }
    inline nlohmann::json mockDoorPassenger(int id, const char* state, const char* safety)
    {
        return {{"id", id}, {"x", 0.25}, {"y", 0.8}, {"state", state},
            {"safety_state", safety ? nlohmann::json(safety) : nlohmann::json(nullptr)},
            {"label", "MOCK policy input"}};
    }
    // Session steady-clock script time, not sensor/processing latency.
    // Only risk-clear emits a timed clear; absence is not inferred as departure.
    inline std::optional<nlohmann::json> mockDoorScenarioInput(
        std::string_view name, std::chrono::microseconds elapsed)
    {
        if (!isMockDoorScenario(name)) throw std::invalid_argument("invalid synthetic door scenario");
        using Json = nlohmann::json;
        if (name == "input-loss" && elapsed >= std::chrono::seconds(8)) return std::nullopt;
        if (name == "normal" || (name == "risk-clear" && elapsed >= std::chrono::seconds(8)))
            return Json::array();
        if (name == "inside-fall") return Json::array({mockDoorPassenger(1, "INSIDE", "FALL")});
        if (name == "inside-immobile") return Json::array({mockDoorPassenger(1, "INSIDE", "IMMOBILE")});
        if (name == "entering-immobile") return Json::array({mockDoorPassenger(1, "ENTERING", "IMMOBILE")});
        if (name == "exiting-fall") return Json::array({mockDoorPassenger(1, "EXITING", "FALL")});
        if (name == "exiting-immobile") return Json::array({mockDoorPassenger(1, "EXITING", "IMMOBILE")});
        if (name == "multi") return Json::array({mockDoorPassenger(1, "INSIDE", "IMMOBILE"),
            mockDoorPassenger(2, "ENTERING", "FALL"), mockDoorPassenger(3, "OUTSIDE", nullptr)});
        return Json::array({mockDoorPassenger(1, "ENTERING", "FALL")});
    }
}
