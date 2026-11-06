#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

namespace elevsafe
{
    // Retry count is never reset during the process, even after a healthy session.
    // The recovery clock covers waits/initialization/connects, not connected receive time.
    class RadarReconnectBudget
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr std::size_t maximumRetries = 3;
        static constexpr auto delay = std::chrono::seconds(1);
        static constexpr auto recoveryTime = std::chrono::seconds(15);

        std::optional<Clock::time_point> schedule(Clock::time_point now, bool wasConnected)
        {
            if (wasConnected) m_deadline.reset();
            if (!m_deadline) m_deadline = now + recoveryTime;
            if (m_retries >= maximumRetries || now + delay >= *m_deadline) return std::nullopt;
            ++m_retries;
            return now + delay;
        }
        std::size_t retries() const noexcept { return m_retries; }
        std::optional<Clock::time_point> deadline() const noexcept { return m_deadline; }

    private:
        std::size_t m_retries = 0;
        std::optional<Clock::time_point> m_deadline;
    };
}
