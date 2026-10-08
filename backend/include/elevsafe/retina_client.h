#pragma once

#include "retina_protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace elevsafe
{
    class RetinaClient
    {
    public:
        using FrameCallback = std::function<void(const RadarFrame&)>;

        RetinaClient(std::string host, std::uint16_t port, FrameCallback callback = {});
        ~RetinaClient();

        RetinaClient(const RetinaClient&) = delete;
        RetinaClient& operator=(const RetinaClient&) = delete;

        void setFrameCallback(FrameCallback callback);
        void setProcessingMetricsEnabled(bool enabled) noexcept;
        void setProcessingMetricsFrameLimit(std::optional<std::size_t> frameLimit) noexcept;

        // Blocking receive loop. Returns 0 on a clean disconnect, otherwise 1.
        int run(std::ostream& log);
        void stop();

        const ParserProcessingSummary& processingSummary() const noexcept;
        bool processingMetricsFrozen() const noexcept;

    private:
        void closeSocket();

        std::string m_host;
        std::uint16_t m_port;
        FrameCallback m_callback;
        std::intptr_t m_socket = -1;
        bool m_stopRequested = false;
        ParserProcessingSummary m_processingSummary;
        bool m_processingMetricsEnabled = false;
        std::optional<std::size_t> m_processingMetricsFrameLimit;
        bool m_processingMetricsFrozen = false;
    };
}
