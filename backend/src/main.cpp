#include "elevsafe/backend_pipeline.h"
#include "elevsafe/ab_profile.h"
#include "elevsafe/radar_reconnect.h"
#include "elevsafe/retina_client.h"
#include "elevsafe/signal_adapter.h"
#include "pipeline/signal_pipeline.h"
#include "elevsafe/websocket_server.h"
#include "elevsafe/websocket_message.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace
{
    bool parsePort(const char* text, std::uint16_t& port)
    {
        const std::string value(text);
        unsigned int parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{}
            || result.ptr != value.data() + value.size()
            || parsed == 0
            || parsed > 65535)
            return false;

        port = static_cast<std::uint16_t>(parsed);
        return true;
    }

    bool parsePositiveSize(const char* text, std::size_t& value)
    {
        const std::string input(text);
        std::size_t parsed = 0;
        const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
        if (result.ec != std::errc{}
            || result.ptr != input.data() + input.size()
            || parsed == 0)
            return false;

        value = parsed;
        return true;
    }

    void printUsage()
    {
        std::cerr << "Usage: elevsafe_backend [host] [port] [--benchmark] [--benchmark-frames N] [--signal-config PATH]\n"
                     "       [--ab-profile [--ab-profile-max-frames N] [--ab-profile-warmup N]]\n"
                     "       [--radar-reconnect [--radar-reconnect-eof]] (numeric radar IP only)\n"
                     "       [--websocket] [--ws-bind IP] [--ws-port PORT]\n"
                     "       elevsafe_backend --ws-mock [--ws-bind IP] [--ws-port PORT] [--ws-duration SECONDS]\n"
                     "       --mock-websocket is an alias for --ws-mock\n";
    }

    void printProcessingSummary(const elevsafe::ParserProcessingSummary& summary)
    {
        std::cout << std::fixed << std::setprecision(3)
                  << "Parser processing summary:\n"
                  << "totalValidFrames=" << summary.totalValidFrames << '\n'
                  << "warmupExcluded=" << summary.warmupExcluded << '\n'
                  << "measuredFrames=" << summary.measuredFrames << '\n'
                  << "averageProcessingMs=" << summary.averageProcessingMs << '\n'
                  << "p95ProcessingMs=" << summary.p95ProcessingMs << '\n'
                  << "maximumProcessingMs=" << summary.maximumProcessingMs << '\n';
    }

    std::atomic<bool> mockStop{false};
    static_assert(std::atomic<bool>::is_always_lock_free);
    void stopMock(int) { mockStop.store(true, std::memory_order_relaxed); }

    std::atomic<bool> radarStop{false};
    void stopRadar(int) { radarStop.store(true, std::memory_order_relaxed); }
    class RadarSignals
    {
        using Handler = void(*)(int);
        bool enabled;
        Handler interrupt = SIG_ERR, terminate = SIG_ERR;
    public:
        explicit RadarSignals(bool active) : enabled(active)
        {
            if (!enabled) return;
            radarStop.store(false, std::memory_order_relaxed);
            interrupt = std::signal(SIGINT, stopRadar);
            terminate = std::signal(SIGTERM, stopRadar);
            if (interrupt == SIG_ERR || terminate == SIG_ERR)
            {
                if (interrupt != SIG_ERR) std::signal(SIGINT, interrupt);
                if (terminate != SIG_ERR) std::signal(SIGTERM, terminate);
                throw std::runtime_error("Cannot install Radar cancellation handlers");
            }
        }
        ~RadarSignals()
        {
            if (enabled && interrupt != SIG_ERR) std::signal(SIGINT, interrupt);
            if (enabled && terminate != SIG_ERR) std::signal(SIGTERM, terminate);
        }
    };

    int runMock(elevsafe::WebSocketServer& server, std::size_t seconds)
    {
        mockStop.store(false, std::memory_order_relaxed);
        const auto previous = std::signal(SIGINT, stopMock);
        std::cerr << "[ws] MOCK ONLY: synthetic session snapshots, target 100ms (10Hz), duration=" << seconds
                  << "s. No radar/C result, no door control. Ctrl+C stops this mode.\n";
        constexpr auto period = std::chrono::milliseconds(100);
        std::optional<std::chrono::steady_clock::time_point> firstFrame;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto next = std::chrono::steady_clock::now();
        while (!mockStop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next)
            {
                if (!firstFrame) firstFrame = now;
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - *firstFrame).count();
                const auto message = elevsafe::mockSnapshot(static_cast<std::uint64_t>(elapsed));
                if (!server.publishSnapshot(message))
                {
                    std::cerr << "[ws] Mock publish failed\n";
                    if (previous != SIG_ERR) std::signal(SIGINT, previous);
                    return 1;
                }
                next += period;
                // Skip missed slots instead of issuing a catch-up burst.
                const auto afterPublish = std::chrono::steady_clock::now();
                if (next <= afterPublish) next = afterPublish + period;
            }
            while (server.popCommand()) {} // Received requests are logged, never executed.
            std::this_thread::sleep_until(std::min({deadline, next, now + std::chrono::milliseconds(25)}));
        }
        if (previous != SIG_ERR) std::signal(SIGINT, previous);
        server.stop();
        const auto stats = server.statistics();
        std::cerr << "[ws] stopped sent=" << stats.snapshotsSent
                  << " ingressReplaced=" << stats.ingressReplaced
                  << " sessionReplaced=" << stats.sessionReplaced
                  << " commands=" << stats.commandsReceived << " rejected=" << stats.rejectedRequests << '\n';
        return 0;
    }
}

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 29172;
    int positionals = 0;
    bool benchmarkMode = false;
    bool reconnectEnabled = false, reconnectEof = false;
    std::optional<std::size_t> benchmarkFrameLimit;
    bool abProfileEnabled = false, profileLimitSpecified = false, profileWarmupSpecified = false;
    std::size_t profileLimit = 10000, profileWarmup = 5;
    bool websocketEnabled = false;
    bool mockMode = false;
    bool websocketSettings = false;
    bool durationSpecified = false;
    std::size_t mockSeconds = 30;
    elevsafe::WebSocketConfig wsConfig;
    std::optional<std::string> signalConfigPath;
    for (int argument = 1; argument < argc;)
    {
        const std::string_view option(argv[argument]);
        if (option == "--radar-reconnect" || option == "--radar-reconnect-eof")
        {
            auto& value = option == "--radar-reconnect" ? reconnectEnabled : reconnectEof;
            if (value) { std::cerr << "Duplicate " << option << '\n'; return 2; }
            value = true; ++argument; continue;
        }
        if (option == "--ab-profile")
        {
            if (abProfileEnabled) { std::cerr << "Duplicate --ab-profile\n"; return 2; }
            abProfileEnabled = true; ++argument; continue;
        }
        if (option == "--ab-profile-max-frames" || option == "--ab-profile-warmup")
        {
            const bool limit = option == "--ab-profile-max-frames";
            if (argument + 1 >= argc || (limit ? profileLimitSpecified : profileWarmupSpecified))
            { std::cerr << "Missing or duplicate " << option << '\n'; return 2; }
            const std::string_view value(argv[argument + 1]);
            std::size_t parsed = 0;
            const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()
                || (limit && parsed == 0) || parsed > elevsafe::AbProfile::kMaximumSamples)
            { std::cerr << "Invalid " << option << ": maximum 100000; sample limit must be positive\n"; return 2; }
            if (limit) { profileLimit = parsed; profileLimitSpecified = true; }
            else { profileWarmup = parsed; profileWarmupSpecified = true; }
            argument += 2; continue;
        }
        if (option == "--signal-config")
        {
            if (signalConfigPath.has_value() || argument + 1 >= argc
                || std::string_view(argv[argument + 1]).empty()
                || std::string_view(argv[argument + 1]).starts_with("--"))
            {
                std::cerr << "--signal-config requires one non-empty path and may be specified only once.\n";
                printUsage();
                return 2;
            }
            signalConfigPath = argv[argument + 1];
            argument += 2;
            continue;
        }
        if (option == "--websocket" || option == "--ws-mock" || option == "--mock-websocket")
        {
            websocketEnabled = true;
            mockMode = mockMode || option == "--ws-mock" || option == "--mock-websocket";
            ++argument;
            continue;
        }
        if (option == "--ws-port" || option == "--ws-bind" || option == "--ws-duration")
        {
            if (argument + 1 >= argc || std::string_view(argv[argument + 1]).empty()
                || std::string_view(argv[argument + 1]).starts_with("--"))
            {
                std::cerr << "Missing value for " << option << '\n';
                return 2;
            }
            const auto* value = argv[argument + 1];
            if (option == "--ws-port" && !parsePort(value, wsConfig.port))
            {
                std::cerr << "Invalid --ws-port: expected 1-65535\n";
                return 2;
            }
            if (option == "--ws-bind") wsConfig.address = value;
            if (option == "--ws-duration")
            {
                if (!parsePositiveSize(value, mockSeconds) || mockSeconds > 86400)
                {
                    std::cerr << "Invalid --ws-duration: expected 1-86400 seconds\n";
                    return 2;
                }
                durationSpecified = true;
            }
            websocketSettings = true;
            argument += 2;
            continue;
        }
        if (option == "--benchmark")
        {
            benchmarkMode = true;
            ++argument;
            continue;
        }

        if (option == "--benchmark-frames")
        {
            if (argument + 1 >= argc)
            {
                std::cerr << "Missing value for --benchmark-frames.\n";
                printUsage();
                return 2;
            }

            if (benchmarkFrameLimit.has_value())
            {
                std::cerr << "--benchmark-frames may be specified only once.\n";
                printUsage();
                return 2;
            }

            std::size_t frameLimit = 0;
            if (!parsePositiveSize(argv[argument + 1], frameLimit))
            {
                std::cerr << "Invalid --benchmark-frames value '" << argv[argument + 1]
                          << "'. It must be a positive integer.\n";
                printUsage();
                return 2;
            }

            benchmarkFrameLimit = frameLimit;
            argument += 2;
            continue;
        }

        if (option.starts_with("--") || positionals >= 2)
        {
            std::cerr << "Unknown option or extra argument '" << option << "'.\n";
            printUsage();
            return 2;
        }
        if (positionals == 0) host = option;
        else if (!parsePort(argv[argument], port))
        {
            std::cerr << "Invalid port '" << option << "'. Port must be in the range 1-65535.\n";
            return 2;
        }
        ++positionals;
        ++argument;
    }

    if ((websocketSettings && !websocketEnabled) || (durationSpecified && !mockMode)
        || (mockMode && (positionals != 0 || benchmarkMode || benchmarkFrameLimit.has_value() || signalConfigPath.has_value())))
    {
        std::cerr << "WebSocket settings require --websocket/--ws-mock; duration is Mock-only. "
                     "Mock cannot be combined with radar host/port, benchmark or --signal-config options.\n";
        return 2;
    }

    if (benchmarkFrameLimit.has_value() && !benchmarkMode)
    {
        std::cerr << "--benchmark-frames requires --benchmark.\n";
        printUsage();
        return 2;
    }

    if (((profileLimitSpecified || profileWarmupSpecified) && !abProfileEnabled)
        || (abProfileEnabled && (mockMode || benchmarkMode || !signalConfigPath)))
    {
        std::cerr << "--ab-profile requires --signal-config; cannot combine with Mock or --benchmark. "
                     "Profile settings require --ab-profile.\n";
        return 2;
    }

    if ((reconnectEof && !reconnectEnabled) || (reconnectEnabled && mockMode))
    {
        std::cerr << "--radar-reconnect-eof requires --radar-reconnect; reconnect is Radar-only\n";
        return 2;
    }

    std::optional<elevsafe::sp::Config> sessionConfig;
    std::unique_ptr<elevsafe::sp::SignalPipeline> signal;
    if (signalConfigPath.has_value())
    {
        try
        {
            elevsafe::sp::Config config;
            if (!config.load(*signalConfigPath))
            {
                std::cerr << "Signal config load failed: " << *signalConfigPath << '\n';
                return 1;
            }
            sessionConfig = config;
            signal = std::make_unique<elevsafe::sp::SignalPipeline>(*sessionConfig);
            if (!signal->ok())
            {
                std::cerr << "SignalPipeline initialization failed: invalid config: " << *signalConfigPath << '\n';
                return 1;
            }
            std::cerr << "SignalPipeline ready: config=" << *signalConfigPath
                      << " background=" << (signal->usesBackground() ? "enabled" : "disabled") << '\n';
        }
        catch (const std::exception& exception)
        {
            std::cerr << "SignalPipeline initialization failed: " << exception.what() << '\n';
            return 1;
        }
    }

    std::unique_ptr<elevsafe::AbProfile> abProfile;
    if (abProfileEnabled)
    {
        try { abProfile = std::make_unique<elevsafe::AbProfile>(profileLimit, profileWarmup); }
        catch (const std::exception& error)
        { std::cerr << "A-B profile initialization failed: " << error.what() << '\n'; return 1; }
    }
    const bool quietFrames = benchmarkMode || abProfileEnabled;

    std::unique_ptr<elevsafe::WebSocketServer> websocket;
    if (websocketEnabled)
    {
        try
        {
            auto candidate = std::make_unique<elevsafe::WebSocketServer>(wsConfig, std::cerr);
            candidate->start();
            websocket = std::move(candidate);
        }
        catch (const std::bad_alloc& error)
        {
            std::cerr << "[ws] fatal allocation failure: " << error.what() << '\n';
            return 1;
        }
        catch (const std::system_error& error)
        {
            // Standard-library thread/mutex failures are not optional transport failures.
            std::cerr << "[ws] fatal runtime failure: " << error.what() << '\n';
            return 1;
        }
        catch (const std::exception& error)
        {
            std::cerr << (mockMode ? "[ws] startup/runtime failed: " : "[ws] startup failed: ")
                      << error.what() << '\n';
            if (mockMode) return 1;
            std::cerr << "[ws] disabled for this run; continuing radar processing\n";
        }
        if (mockMode)
        {
            try { return runMock(*websocket, mockSeconds); }
            catch (const std::exception& error)
            {
                std::cerr << "[ws] startup/runtime failed: " << error.what() << '\n';
                return 1;
            }
        }
        if (websocket)
            std::cerr << "[ws] transport only: no C result, no live snapshot generation or door control\n";
    }

    // WebSocket remains outside the Radar session lifetime; no live C snapshots are invented.
    const bool initialBackground = signal && signal->usesBackground();
    int result = 1;
    try
    {
        RadarSignals signals(reconnectEnabled);
        elevsafe::RadarReconnectBudget retryBudget;
        for (std::size_t session = 1;; ++session)
        {
            if (reconnectEnabled && radarStop.load(std::memory_order_relaxed)) return 0;
            if (session > 1)
            {
                // Fresh B history/statistics and background load; do not modify B's public API.
                if (sessionConfig)
                {
                    signal = std::make_unique<elevsafe::sp::SignalPipeline>(*sessionConfig);
                    if (!signal->ok()) throw std::runtime_error("SignalPipeline reinitialization failed");
                    if (signal->usesBackground() != initialBackground)
                        throw std::runtime_error("Background activation changed across Radar sessions");
                    std::cerr << "SignalPipeline ready: config=" << *signalConfigPath
                              << " background=" << (signal->usesBackground() ? "enabled" : "disabled") << '\n';
                }
                if (abProfileEnabled) abProfile = std::make_unique<elevsafe::AbProfile>(profileLimit, profileWarmup);
            }
            if (reconnectEnabled) std::cerr << "[radar] session=" << session << " begin\n";
            // Capacity 1 is only the minimum needed for synchronous enqueue -> immediate drain.
            // It is not a production tuning value and must be revisited for a separated producer/consumer design.
            constexpr std::size_t kSynchronousFrameBufferCapacity = 1;
            elevsafe::BackendPipeline pipeline(kSynchronousFrameBufferCapacity);

            elevsafe::RetinaClient client(host, port);
            client.setProcessingMetricsEnabled(benchmarkMode);
            client.setProcessingMetricsFrameLimit(benchmarkFrameLimit);

            bool summaryPrinted = false;
            std::optional<std::uint64_t> firstTimestamp;
            auto lastDoorState = elevsafe::sp::DoorState::UNKNOWN;
            client.setFrameCallback([&pipeline, &client, &signal, &abProfile, benchmarkMode, quietFrames,
                &summaryPrinted, reconnectEnabled, &firstTimestamp, &lastDoorState](const elevsafe::RadarFrame& frame)
            {
                if (reconnectEnabled && !firstTimestamp) firstTimestamp = frame.timestampUs;
                using Profile = elevsafe::AbProfile;
                std::optional<Profile::FrameScope> profiled;
                if (abProfile)
                    profiled.emplace(*abProfile, frame.frameCount, frame.points.size(), pipeline.pendingSize() >= kSynchronousFrameBufferCapacity);
                const auto pipelineStart = profiled ? Profile::Clock::now() : Profile::Clock::time_point{};
                pipeline.onFrame(frame);
                pipeline.drain([&signal, &profiled, quietFrames, reconnectEnabled, &lastDoorState](const elevsafe::RadarFrame& pendingFrame)
                {
                    if (signal)
                    {
                        try
                        {
                            const auto adapterStart = profiled ? Profile::Clock::now() : Profile::Clock::time_point{};
                            const auto input = elevsafe::toSignalFrame(pendingFrame);
                            if (profiled) profiled->sample.adapter = Profile::Clock::now() - adapterStart;
                            const elevsafe::sp::FrameContext context;
                            const auto bStart = profiled ? Profile::Clock::now() : Profile::Clock::time_point{};
                            const auto result = signal->process(input, context);
                            if (reconnectEnabled) lastDoorState = result.estimated_door_state;
                            if (profiled)
                            {
                                const auto bEnd = Profile::Clock::now();
                                profiled->sample.b = bEnd - bStart;
                                profiled->sample.ab = bEnd - adapterStart;
                                profiled->sample.success = true;
                            }
                            if (!quietFrames)
                                std::cout << "signal frame_id=" << result.frame_id
                                          << " timestamp_us=" << result.timestamp_us
                                          << " inputPoints=" << input.points.size()
                                          << " clusters=" << result.clusters.size()
                                          << " estimated_door_state="
                                          << elevsafe::sp::doorStateToString(result.estimated_door_state) << '\n';
                        }
                        catch (const std::bad_alloc&)
                        {
                            throw;
                        }
                        catch (const std::exception& exception)
                        {
                            // drain already owns/popped this frame; skip it without retrying B's stateful process.
                            std::cerr << "Signal frame failed: frameCount=" << pendingFrame.frameCount
                                      << " timestampUs=" << pendingFrame.timestampUs
                                      << ": " << exception.what() << '\n';
                            if (reconnectEnabled) throw; // Processing errors never trigger transport recovery.
                        }
                    }

                    if (quietFrames)
                        return;

                    std::cout << "frameCount=" << pendingFrame.frameCount
                              << " timestampUs=" << pendingFrame.timestampUs
                              << " pointCount=" << pendingFrame.points.size();

                    if (!pendingFrame.points.empty())
                    {
                        const auto& point = pendingFrame.points.front();
                        std::cout << std::fixed << std::setprecision(3)
                                  << " firstPoint=(x=" << point.x
                                  << ", y=" << point.y
                                  << ", z=" << point.z
                                  << ", doppler=" << point.doppler
                                  << ", power=" << point.power
                                  << ", targetId=" << point.targetId
                                  << ')';
                    }

                    std::cout << '\n';
                });
                if (profiled) profiled->sample.pipeline = Profile::Clock::now() - pipelineStart;

                if (benchmarkMode && !summaryPrinted && client.processingMetricsFrozen())
                {
                    printProcessingSummary(client.processingSummary());
                    summaryPrinted = true;
                }
            });

            std::cerr << "Connecting to " << host << ':' << port << "...\n";
            result = 1;
            elevsafe::RetinaClient::RunDiagnostics profileDiagnostics;
            bool runReturned = false;
            try
            {
                if (reconnectEnabled)
                {
                    elevsafe::RetinaClient::SessionControl control;
                    control.connectNotAfter = retryBudget.deadline();
                    control.cancelRequested = [] { return radarStop.load(std::memory_order_relaxed); };
                    result = client.runSession(std::cerr, control, profileDiagnostics);
                }
                else result = abProfile ? client.run(std::cerr, {}, profileDiagnostics) : client.run(std::cerr);
                runReturned = true;
            }
            catch (const std::exception& exception)
            {
                if (reconnectEnabled) profileDiagnostics.stage = "processing_exception";
                std::cerr << "Backend stopped: " << exception.what() << '\n';
            }

            if (benchmarkMode && !summaryPrinted)
                printProcessingSummary(client.processingSummary());

            if (abProfile)
            {
                std::cout << "AB_PROFILE_JSON {\"mode\":\"tcp_callback\",\"background_enabled\":"
                          << (signal->usesBackground() ? "true" : "false");
                if (reconnectEnabled) std::cout << ",\"session_id\":" << session;
                std::cout << ",\"run_returned\":" << (runReturned ? "true" : "false")
                          << ",\"clean_eof\":" << (runReturned && result == 0 && profileDiagnostics.stage == "eof" ? "true" : "false")
                          << ",\"exit_code\":" << result << ",\"termination_stage\":\""
                          << (runReturned ? profileDiagnostics.stage : "processing_exception")
                          << "\",\"socket_error_code\":" << profileDiagnostics.errorCode
                          << ",\"parser_statistics_finalized\":" << (runReturned ? "true" : "false")
                          << ",\"parser_packets_parsed\":";
                if (runReturned) std::cout << profileDiagnostics.parser.packetsParsed; else std::cout << "null";
                std::cout << ",\"parser_packets_rejected\":";
                if (runReturned) std::cout << profileDiagnostics.parser.packetsRejected; else std::cout << "null";
                std::cout << ",\"parser_buffered_bytes\":";
                if (runReturned) std::cout << profileDiagnostics.bufferedBytes; else std::cout << "null";
                std::cout << ",\"metrics\":";
                abProfile->writeJson(std::cout);
                std::cout << "}\n";
            }

            if (reconnectEnabled)
            {
                std::cerr << "[radar] session=" << session << " end stage=" << profileDiagnostics.stage
                          << " code=" << profileDiagnostics.errorCode
                          << " connected=" << profileDiagnostics.connected
                          << " parsed=" << profileDiagnostics.parser.packetsParsed
                          << " rejected=" << profileDiagnostics.parser.packetsRejected
                          << " buffered=" << profileDiagnostics.bufferedBytes;
                if (signal) std::cerr << " B_frames=" << signal->stats().frames
                                      << " background=" << signal->usesBackground()
                                      << " last_door_state=" << elevsafe::sp::doorStateToString(lastDoorState);
                if (firstTimestamp) std::cerr << " first_timestamp_us=" << *firstTimestamp;
                std::cerr << '\n';
            }
            if (!reconnectEnabled || !runReturned) break;
            if (radarStop.load(std::memory_order_relaxed) || profileDiagnostics.cancelled)
            { result = 0; break; }
            if (!elevsafe::RetinaClient::retryable(profileDiagnostics, reconnectEof)) break;
            const auto next = retryBudget.schedule(elevsafe::RadarReconnectBudget::Clock::now(), profileDiagnostics.connected);
            if (!next)
            {
                std::cerr << "[radar] recovery exhausted retries=" << retryBudget.retries() << " (limit=3, budget=15s)\n";
                result = 1; break;
            }
            std::cerr << "[radar] retry=" << retryBudget.retries() << "/3 wait_ms=1000 connect_timeout_ms=3000\n";
            while (elevsafe::RadarReconnectBudget::Clock::now() < *next && !radarStop.load(std::memory_order_relaxed))
                std::this_thread::sleep_until(std::min(*next,
                    elevsafe::RadarReconnectBudget::Clock::now() + std::chrono::milliseconds(50)));
            if (radarStop.load(std::memory_order_relaxed))
            {
                std::cerr << "[radar] cancelled during retry wait\n";
                return 0;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Radar session initialization/reporting failed: " << error.what() << '\n';
        return 1;
    }
    return result;
}
