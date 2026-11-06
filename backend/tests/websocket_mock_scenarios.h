#pragma once

#include "elevsafe/websocket_message.h"

#include <nlohmann/json.hpp>
#include <vector>

namespace elevsafe::test
{
    struct MockScenario
    {
        const char* name;
        nlohmann::json snapshot;
    };

    // Test data only: no tracking, state estimation or product Mock changes.
    inline std::vector<MockScenario> mockScenarios()
    {
        using Json = nlohmann::json;
        auto frame = Json::parse(mockSnapshot());
        std::vector<MockScenario> scenarios;
        frame["passengers"] = Json::array();
        frame["door_state"] = "CLOSED";
        scenarios.push_back({"empty", frame});

        frame["timestamp_us"] = 100000;
        frame["door_state"] = "OPENING";
        frame["door_command"] = "OPEN";
        frame["passengers"] = Json::array({{
            {"id", 101}, {"x", -0.3}, {"y", -0.7}, {"state", "OUTSIDE"},
            {"safety_state", nullptr}, {"label", "MOCK Passenger #101"}
        }});
        scenarios.push_back({"one-outside", frame});

        frame["timestamp_us"] = 200000;
        frame["door_state"] = "OPEN";
        frame["door_command"] = nullptr;
        frame["passengers"][0]["x"] = -0.1;
        frame["passengers"][0]["y"] = 0.05;
        frame["passengers"][0]["state"] = "ENTERING";
        frame["passengers"].push_back({
            {"id", 202}, {"x", 0.5}, {"y", 0.9}, {"state", "INSIDE"},
            {"safety_state", "FALL"}, {"label", "MOCK Passenger #202"}
        });
        scenarios.push_back({"two-entering-fall", frame});

        frame["timestamp_us"] = 300000;
        frame["door_state"] = "CLOSING";
        frame["door_command"] = "HOLD";
        frame["passengers"][0]["x"] = 0.2;
        frame["passengers"][0]["y"] = 0.8;
        frame["passengers"][0]["state"] = "INSIDE";
        frame["passengers"][0]["safety_state"] = "IMMOBILE";
        frame["passengers"][1]["y"] = -0.05;
        frame["passengers"][1]["state"] = "EXITING";
        frame["passengers"][1]["safety_state"] = nullptr;
        scenarios.push_back({"two-moving-immobile-exiting", frame});

        frame["timestamp_us"] = 400000;
        frame["door_state"] = "CLOSED";
        frame["door_command"] = "CLOSE";
        // Whole-list replacement; old IDs must not leak into either client.
        // Optional label is deliberately absent in this synthetic sample.
        frame["passengers"] = Json::array({{
            {"id", "MOCK-303"}, {"x", -0.4}, {"y", -0.8}, {"state", "OUTSIDE"},
            {"safety_state", nullptr}
        }});
        scenarios.push_back({"replacement-id-no-label", frame});

        frame["timestamp_us"] = 500000;
        frame["door_state"] = "OPEN";
        frame["door_command"] = nullptr;
        frame["passengers"] = Json::array();
        scenarios.push_back({"empty-again", frame});
        return scenarios;
    }
}
