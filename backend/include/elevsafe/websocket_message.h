#pragma once

#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

namespace elevsafe
{
    bool isSnapshot(std::string_view text);
    std::optional<std::string> parseDoorRequest(std::string_view text);
    // Strictly one null field. Caller must explicitly opt into no-op compatibility.
    bool isNullDoorRequest(std::string_view text);
    std::string mockSnapshot();
    // Synthetic session elapsed microseconds, not sensor time or processing latency.
    std::string mockSnapshot(std::uint64_t timestampUs);
}
