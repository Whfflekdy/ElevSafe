#include "elevsafe/websocket_message.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void expect(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
}

int main()
{
    try
    {
        using Json = nlohmann::json;
        auto json = Json::parse(elevsafe::mockSnapshot());
        expect(elevsafe::isSnapshot(json.dump()), "mock snapshot must be valid");
        expect(json.size() == 5 && json["passengers"][0]["label"] == "MOCK Passenger #1", "mock format mismatch");
        expect(json["timestamp_us"] == 0 && !json.contains("alert"), "v6 initial timestamp/fields mismatch");
        for (const auto value : {std::uint64_t{9382050}, std::numeric_limits<std::uint64_t>::max()})
        {
            const auto timed = Json::parse(elevsafe::mockSnapshot(value));
            expect(timed["timestamp_us"].get<std::uint64_t>() == value, "timestamp integer precision lost");
            expect(elevsafe::isSnapshot(timed.dump()), "valid elapsed timestamp rejected");
            expect(timed["timestamp"] == json["timestamp"], "fixed mock timestamp changed");
        }
        auto invalid = json;
        invalid.erase("timestamp_us");
        expect(!elevsafe::isSnapshot(invalid.dump()), "missing v6 timestamp accepted");
        for (const auto& value : Json::array({-1, 0.5, 1.0, "100000", nullptr, true}))
        {
            invalid = json;
            invalid["timestamp_us"] = value;
            expect(!elevsafe::isSnapshot(invalid.dump()), "invalid elapsed timestamp accepted");
        }
        invalid = json;
        invalid["alert"] = true;
        expect(!elevsafe::isSnapshot(invalid.dump()), "separate alert field accepted");
        json["passengers"][0].erase("label");
        expect(elevsafe::isSnapshot(json.dump()), "label must be optional");
        json["passengers"][0]["x"] = "NaN";
        expect(!elevsafe::isSnapshot(json.dump()), "non-numeric coordinate accepted");
        json["passengers"][0]["x"] = nullptr;
        expect(!elevsafe::isSnapshot(json.dump()), "non-finite/null coordinate accepted");
        json["passengers"] = Json::array();
        expect(elevsafe::isSnapshot(json.dump()), "empty snapshot must be valid");
        json["door_state"] = "UNKNOWN";
        expect(!elevsafe::isSnapshot(json.dump()), "UNKNOWN must not be converted to a valid door state");
        expect(!elevsafe::isSnapshot("{\"timestamp\":1e9999}"), "overflowing JSON number accepted");
        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
        {
            const auto parsed = elevsafe::parseDoorRequest(Json({{"door_command", command}}).dump());
            expect(parsed.has_value(), "valid command rejected");
            expect(*parsed == command, "command changed");
        }
        for (const auto* text : {"{", "[]", "null", "{}", "{\"door_command\":null}",
            "{\"door_command\":\"AUTO\"}", "{\"door_command\":\"OPEN\",\"execute\":true}"})
            expect(!elevsafe::parseDoorRequest(text).has_value(), "invalid command accepted");
    }
    catch (const std::exception& error)
    {
        std::cerr << "websocket_message_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "websocket_message_test passed\n";
}
