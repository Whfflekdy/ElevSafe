#include "elevsafe/websocket_message.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <initializer_list>

namespace elevsafe
{
    namespace
    {
        using Json = nlohmann::json;

        bool oneOf(const Json& value, std::initializer_list<const char*> allowed)
        {
            if (!value.is_string())
                return false;
            for (const auto* candidate : allowed)
                if (value == candidate)
                    return true;
            return false;
        }
    }

    bool isSnapshot(std::string_view text)
    {
        const auto json = Json::parse(text, nullptr, false);
        if (!json.is_object() || json.size() != 5
            || !json.contains("timestamp") || !json["timestamp"].is_string()
            || !json.contains("timestamp_us")
            || !(json["timestamp_us"].is_number_unsigned()
                || (json["timestamp_us"].is_number_integer() && json["timestamp_us"].get<std::int64_t>() >= 0))
            || !json.contains("door_state") || !oneOf(json["door_state"], {"OPEN", "CLOSED", "OPENING", "CLOSING"})
            || !json.contains("door_command")
            || !(json["door_command"].is_null() || oneOf(json["door_command"], {"OPEN", "HOLD", "CLOSE"}))
            || !json.contains("passengers") || !json["passengers"].is_array())
            return false;
        for (const auto& point : json["passengers"])
        {
            if (!point.is_object() || !point.contains("id")
                || !(point["id"].is_number_integer() || point["id"].is_string())
                || !point.contains("x") || !point["x"].is_number()
                || !point.contains("y") || !point["y"].is_number()
                || !std::isfinite(point["x"].get<double>()) || !std::isfinite(point["y"].get<double>())
                || !point.contains("state") || !oneOf(point["state"], {"OUTSIDE", "ENTERING", "INSIDE", "EXITING"})
                || !point.contains("safety_state")
                || !(point["safety_state"].is_null() || oneOf(point["safety_state"], {"FALL", "IMMOBILE"}))
                || (point.contains("label") && !point["label"].is_string()))
                return false;
            for (const auto& item : point.items())
                if (item.key() != "id" && item.key() != "x" && item.key() != "y" && item.key() != "state"
                    && item.key() != "safety_state" && item.key() != "label")
                    return false;
        }
        return true;
    }

    std::optional<std::string> parseDoorRequest(std::string_view text)
    {
        const auto json = Json::parse(text, nullptr, false);
        if (!json.is_object() || json.size() != 1 || !json.contains("door_command")
            || !oneOf(json["door_command"], {"OPEN", "HOLD", "CLOSE"}))
            return std::nullopt;
        return json["door_command"].get<std::string>();
    }

    std::string mockSnapshot()
    {
        return mockSnapshot(0);
    }

    std::string mockSnapshot(std::uint64_t timestampUs)
    {
        // Deliberately fixed example time/state, never a live sensor observation.
        const Json json = {
            {"timestamp", "2026-10-30T11:00:00Z"}, {"door_command", nullptr}, {"door_state", "OPEN"},
            {"timestamp_us", timestampUs},
            {"passengers", Json::array({{
                {"id", 1}, {"x", 0.25}, {"y", 0.8}, {"state", "INSIDE"},
                {"safety_state", nullptr}, {"label", "MOCK Passenger #1"}
            }})}
        };
        return json.dump();
    }
}
