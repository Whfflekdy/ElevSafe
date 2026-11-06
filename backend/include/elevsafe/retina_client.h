#pragma once

#include "retina_protocol.h"

#include <cstddef>
#include <chrono>
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
        struct RunDiagnostics
        {
            std::string stage = "not_started";
            int errorCode = 0;
            bool timedOut = false;
            bool connected = false;
            bool cancelled = false;
            RetinaStreamParser::Statistics parser;
            std::size_t bufferedBytes = 0;
        };
        using Deadline = std::function<std::chrono::steady_clock::time_point()>;
        // Validation-only bounded I/O. Numeric IP; deadline and callback execute
        // on this calling thread. stop() must also be called on this thread.
        // The original run(log) keeps its blocking socket behavior.
        int run(std::ostream& log, const Deadline& deadline, RunDiagnostics& diagnostics);
        struct SessionControl
        {
            std::chrono::milliseconds connectTimeout{3000};
            std::optional<std::chrono::steady_clock::time_point> connectNotAfter;
            // Polled on the owner thread; the predicate may read a lock-free stop flag.
            std::function<bool()> cancelRequested;
        };
        // Numeric IP only: bounded connect without DNS; cancellable receive, no idle timeout.
        int runSession(std::ostream& log, const SessionControl& control, RunDiagnostics& diagnostics);
        static bool retryable(const RunDiagnostics& diagnostics, bool reconnectEof) noexcept;
        void stop();

        const ParserProcessingSummary& processingSummary() const noexcept;
        bool processingMetricsFrozen() const noexcept;

    private:
        void closeSocket();
        int runImpl(std::ostream& log, const Deadline& deadline,
            const SessionControl* control, RunDiagnostics& diagnostics);

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
