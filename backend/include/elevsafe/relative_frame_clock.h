#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace elevsafe
{
    class RelativeFrameClock
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        std::uint64_t timestampUs(TimePoint now) noexcept
        {
            if (!m_origin.has_value())
            {
                m_origin = now;
                return 0;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - *m_origin);
            return static_cast<std::uint64_t>(elapsed.count());
        }

        void reset() noexcept
        {
            m_origin.reset();
        }

    private:
        std::optional<TimePoint> m_origin;
    };
}
