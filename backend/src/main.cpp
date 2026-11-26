#include "elevsafe/backend_pipeline.h"
#include "elevsafe/door_simulation.h"
#include "elevsafe/emulator_door_arbitration.h"
#include "elevsafe/mock_door_policy.h"
#include "elevsafe/periodic_radar_publisher.h"
#include <nlohmann/json.hpp>
#include "elevsafe/ab_profile.h"
#include "elevsafe/radar_reconnect.h"
#include "elevsafe/retina_client.h"
#include "elevsafe/signal_adapter.h"
#include "elevsafe/tracking_adapter.h"
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
#include <cstdlib>
#include <limits>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <syncstream>
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
                     "       [--radar-reconnect [--radar-reconnect-eof] [--radar-idle-timeout-ms N]] (numeric radar IP only)\n"
                     "       [--emulator-door-sim] (Radar TCP, numeric IP, --signal-config --websocket, loopback WS)\n"
                     "       [--emulator-auto-cycle] (requires --emulator-door-sim; one virtual arrival per process)\n"
                     "       [--websocket] [--ws-bind IP] [--ws-port PORT]\n"
                     "       [--ws-remote-control] (loopback, software door mode, CONTROL_LOCAL_TOKEN)\n"
                     "       [--ws-periodic-snapshot] (Radar TCP, --signal-config --websocket)\n"
                     "       elevsafe_backend --ws-mock [--ws-bind IP] [--ws-port PORT] [--ws-duration SECONDS] [--ws-door-sim [--ws-door-scenario NAME]]\n"
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

    void logManualDoor(const elevsafe::ManualDoorRequest& request, const elevsafe::DoorSimulation& door,
        bool applied, std::size_t session)
    {
        using Door = elevsafe::DoorSimulation;
        std::osyncstream(std::cerr) << "[door-sim] manual session=" << session
            << " client=" << request.clientId << " command=" << request.command
            << " decision=" << (applied ? "applied" : "unchanged")
            << " state=" << Door::name(door.state())
            << " mode=" << (door.mode() == Door::Mode::Auto ? "AUTO" : "MANUAL")
            << " source=" << (request.requestId.empty() ? "local" : "remote")
            << " request=" << (request.requestId.empty() ? "-" : request.requestId) << '\n';
    }

    elevsafe::DoorSimulation::Command manualCommand(std::string_view text)
    {
        using Command = elevsafe::DoorSimulation::Command;
        return text == "OPEN" ? Command::Open : text == "HOLD" ? Command::Hold : Command::Close;
    }

    // The applied motion/HOLD belongs to the door. The authenticated socket owns
    // permission to submit more requests. Only the door owner calls this function.
    template <typename Apply>
    bool serviceManualDoor(elevsafe::WebSocketServer& server, elevsafe::DoorSimulation& door,
        std::size_t limit, std::size_t session, const Apply& apply)
    {
        using Door = elevsafe::DoorSimulation;
        std::optional<elevsafe::ManualDoorRequest> remote;
        const bool applied = server.applyRemoteDoor([&](const auto& request, bool sameOwner)
        {
            remote = request;
            const auto command = manualCommand(request.command);
            if (door.mode() == Door::Mode::Manual && door.activeCommand() == command)
            {
                if (sameOwner) return false;
                door.recordManualTakeover();
                return true;
            }
            return apply(command);
        }, door.mode() == Door::Mode::Auto);
        if (remote) logManualDoor(*remote, door, applied, session);
        bool consumed = remote.has_value();
        for (std::size_t i = 0; i < limit; ++i)
        {
            const auto request = server.popCommand();
            if (!request) break;
            consumed = true;
            logManualDoor(*request, door, apply(manualCommand(request->command)), session);
        }
        return consumed;
    }

    std::string commandEnvironment(const char* name)
    {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0) throw std::runtime_error("Could not read control configuration");
        std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
        return value ? value : "";
#else
        const auto* value = std::getenv(name);
        return value ? value : "";
#endif
    }

    int runMock(elevsafe::WebSocketServer& server, std::size_t seconds, bool doorSim,
        const std::optional<std::string>& scenario, bool remoteControl)
    {
        mockStop.store(false, std::memory_order_relaxed);
        const auto previous = std::signal(SIGINT, stopMock);
        std::cerr << "[ws] MOCK ONLY: synthetic session snapshots, target 100ms (10Hz), duration=" << seconds
                  << "s. No radar/C result, no physical door control. Ctrl+C stops this mode.\n";
        elevsafe::DoorSimulation simulation(scenario == "unknown"
            ? elevsafe::DoorSimulation::State::Unknown : elevsafe::DoorSimulation::State::Closed);
        elevsafe::MockDoorPolicy policy;
        const auto scenarioStart = std::chrono::steady_clock::now();
        bool missingReported = false;
        if (scenario) std::cerr << "[door-mock-policy] SYNTHETIC ONLY scenario=" << *scenario << '\n';
        std::optional<elevsafe::DoorSimulation::Command> newlyApplied;
        if (doorSim) std::cerr << "[door-sim] SOFTWARE ONLY mode=AUTO state="
            << elevsafe::DoorSimulation::name(simulation.state()) << "; no hardware/C input/ACK\n";
        const auto logSimulation = [&](const char* event)
        {
            std::cerr << "[door-sim] " << event << " state=" << elevsafe::DoorSimulation::name(simulation.state())
                      << " mode=" << (simulation.mode() == elevsafe::DoorSimulation::Mode::Auto ? "AUTO" : "MANUAL")
                      << " monotonicUs=" << std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
        };
        constexpr auto period = std::chrono::milliseconds(100);
        std::optional<std::chrono::steady_clock::time_point> firstFrame;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto next = std::chrono::steady_clock::now();
        while (!mockStop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (doorSim)
            {
                if (remoteControl)
                {
                    simulation.advanceMotion(now);
                    server.updateRemoteDoor(true, simulation.state() == elevsafe::DoorSimulation::State::Unknown
                        ? "DOOR_UNKNOWN" : "");
                }
                serviceManualDoor(server, simulation, std::numeric_limits<std::size_t>::max(), 0,
                    [&](auto command) { return simulation.apply(command, now); });
                if (scenario)
                {
                    policy.accept(elevsafe::mockDoorScenarioInput(*scenario,
                        std::chrono::duration_cast<std::chrono::microseconds>(now - scenarioStart)));
                    if (policy.inputMissing() && !missingReported)
                        std::cerr << "[door-mock-policy] WARNING input missing/invalid; retaining last valid synthetic input; not risk-clear\n";
                    missingReported = policy.inputMissing();
                    if (policy.tick(simulation, now)) logSimulation("mock-policy-updated");
                }
                else if (simulation.update(now)) logSimulation("state-updated");
                if (const auto applied = simulation.takeAppliedCommand())
                {
                    newlyApplied = applied;
                    logSimulation("command-event");
                }
            }
            if (now >= next)
            {
                if (!firstFrame) firstFrame = now;
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - *firstFrame).count();
                auto message = elevsafe::mockSnapshot(static_cast<std::uint64_t>(elapsed));
                if (doorSim)
                {
                    auto snapshot = nlohmann::json::parse(message);
                    if (scenario) snapshot["passengers"] = policy.passengers();
                    snapshot["door_state"] = elevsafe::DoorSimulation::name(simulation.state());
                    if (newlyApplied) snapshot["door_command"] = elevsafe::DoorSimulation::name(*newlyApplied);
                    message = snapshot.dump();
                }
                if (!server.publishSnapshot(message))
                {
                    std::cerr << "[ws] Mock publish failed\n";
                    if (previous != SIG_ERR) std::signal(SIGINT, previous);
                    return 1;
                }
                newlyApplied.reset();
                next += period;
                // Skip missed slots instead of issuing a catch-up burst.
                const auto afterPublish = std::chrono::steady_clock::now();
                if (next <= afterPublish) next = afterPublish + period;
            }
            if (!doorSim) while (server.popCommand()) {} // Received requests are logged, never executed.
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
    std::optional<std::chrono::milliseconds> radarIdleTimeout;
    bool emulatorDoorSim = false;
    bool emulatorAutoCycle = false;
    std::optional<std::size_t> benchmarkFrameLimit;
    bool abProfileEnabled = false, profileLimitSpecified = false, profileWarmupSpecified = false;
    std::size_t profileLimit = 10000, profileWarmup = 5;
    bool websocketEnabled = false;
    bool periodicSnapshot = false;
    bool remoteControl = false;
    bool mockMode = false;
    bool doorSim = false;
    std::optional<std::string> doorScenario;
    bool websocketSettings = false;
    bool durationSpecified = false;
    std::size_t mockSeconds = 30;
    elevsafe::WebSocketConfig wsConfig;
    std::optional<std::string> signalConfigPath;
    for (int argument = 1; argument < argc;)
    {
        const std::string_view option(argv[argument]);
        if (option == "--ws-door-scenario")
        {
            if (doorScenario || argument + 1 >= argc || !elevsafe::isMockDoorScenario(argv[argument + 1]))
            { std::cerr << "Missing, duplicate or invalid --ws-door-scenario\n"; return 2; }
            doorScenario = argv[argument + 1]; argument += 2; continue;
        }
        if (option == "--ws-remote-control")
        {
            if (remoteControl) { std::cerr << "Duplicate --ws-remote-control\n"; return 2; }
            remoteControl = true; ++argument; continue;
        }
        if (option == "--ws-periodic-snapshot")
        {
            if (periodicSnapshot) { std::cerr << "Duplicate --ws-periodic-snapshot\n"; return 2; }
            periodicSnapshot = true;
            ++argument;
            continue;
        }
        if (option == "--emulator-auto-cycle")
        {
            if (emulatorAutoCycle) { std::cerr << "Duplicate --emulator-auto-cycle\n"; return 2; }
            emulatorAutoCycle = true;
            ++argument;
            continue;
        }
        if (option == "--emulator-door-sim")
        {
            if (emulatorDoorSim) { std::cerr << "Duplicate --emulator-door-sim\n"; return 2; }
            emulatorDoorSim = true;
            ++argument;
            continue;
        }
        if (option == "--ws-door-sim")
        {
            if (doorSim) { std::cerr << "Duplicate --ws-door-sim\n"; return 2; }
            doorSim = true; ++argument; continue;
        }
        if (option == "--radar-idle-timeout-ms")
        {
            std::size_t milliseconds = 0;
            constexpr auto maxIdleTimeoutMs = static_cast<std::size_t>(
                elevsafe::RetinaClient::SessionControl::maximumReceiveIdleTimeout.count());
            if (radarIdleTimeout || argument + 1 >= argc ||
                !parsePositiveSize(argv[argument + 1], milliseconds) || milliseconds > maxIdleTimeoutMs)
            {
                std::cerr << "Invalid or duplicate --radar-idle-timeout-ms: expected 1-" << maxIdleTimeoutMs << '\n';
                return 2;
            }
            radarIdleTimeout = std::chrono::milliseconds(milliseconds);
            argument += 2;
            continue;
        }
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

    if (remoteControl)
    {
        if ((!emulatorDoorSim && !(mockMode && doorSim)) || benchmarkMode ||
            (wsConfig.address != "127.0.0.1" && wsConfig.address != "::1"))
        {
            std::cerr << "--ws-remote-control requires a loopback software door simulation.\n";
            return 2;
        }
        wsConfig.remoteControlToken = commandEnvironment("CONTROL_LOCAL_TOKEN");
        if (wsConfig.remoteControlToken.size() < 32)
        {
            std::cerr << "Missing or invalid loopback control credential.\n";
            return 2;
        }
        const auto ttl = commandEnvironment("CONTROL_TTL_MS");
        const auto bytes = commandEnvironment("CONTROL_MAX_BYTES");
        std::size_t value{};
        if (!ttl.empty())
        {
            if (!parsePositiveSize(ttl.c_str(), value) || value <= 250 || value > 10000)
            { std::cerr << "Invalid CONTROL_TTL_MS\n"; return 2; }
            wsConfig.remoteControlTtlMs = static_cast<unsigned>(value);
        }
        if (!bytes.empty())
        {
            if (!parsePositiveSize(bytes.c_str(), value) || value < 512 || value > 4096)
            { std::cerr << "Invalid CONTROL_MAX_BYTES\n"; return 2; }
            wsConfig.maxControlBytes = value;
        }
    }
    if (periodicSnapshot && (mockMode || benchmarkMode || !signalConfigPath || !websocketEnabled))
    {
        std::cerr << "--ws-periodic-snapshot requires Radar TCP, --signal-config and --websocket; incompatible with Mock and benchmark.\n";
        return 2;
    }
    if (emulatorAutoCycle && !emulatorDoorSim)
    {
        std::cerr << "--emulator-auto-cycle requires --emulator-door-sim\n";
        return 2;
    }
    if (emulatorDoorSim && (mockMode || benchmarkMode || !signalConfigPath || !websocketEnabled
        || (wsConfig.address != "127.0.0.1" && wsConfig.address != "::1")))
    {
        std::cerr << "--emulator-door-sim requires Radar TCP, --signal-config, --websocket and a loopback WebSocket bind (127.0.0.1 or ::1); incompatible with Mock and benchmark.\n";
        return 2;
    }
    if (doorScenario && (!mockMode || !doorSim))
    {
        std::cerr << "--ws-door-scenario requires --ws-mock --ws-door-sim\n";
        return 2;
    }
    if (doorSim && !mockMode)
    {
        std::cerr << "--ws-door-sim requires --ws-mock (software simulation only)\n";
        return 2;
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

    if (radarIdleTimeout && (!reconnectEnabled || mockMode))
    {
        std::cerr << "--radar-idle-timeout-ms requires --radar-reconnect in Radar mode\n";
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
            wsConfig.allowNullDoorRequest = mockMode && doorSim;
            auto candidate = std::make_unique<elevsafe::WebSocketServer>(wsConfig, std::cerr);
            if (emulatorDoorSim)
            {
                candidate->setManualCommandsEnabled(false);
                if (remoteControl) candidate->updateRemoteDoor(false, "RADAR_NO_OBSERVATION");
            }
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
            if (mockMode || emulatorDoorSim || periodicSnapshot) return 1;
            std::cerr << "[ws] disabled for this run; continuing radar processing\n";
        }
        if (mockMode)
        {
            try { return runMock(*websocket, mockSeconds, doorSim, doorScenario, remoteControl); }
            catch (const std::exception& error)
            {
                std::cerr << "[ws] startup/runtime failed: " << error.what() << '\n';
                return 1;
            }
        }
        if (websocket)
            std::cerr << (signal && !benchmarkMode
                ? "[ws] Tracking: B clusters -> C CONFIRMED with Phase/Safety; no physical door control\n"
                : "[ws] transport only: no C result, no live snapshot generation or door control\n");
    }

    if (emulatorDoorSim)
        std::cerr << "[emulator-door-sim] SOFTWARE ONLY: virtual door starts CLOSED; manual requests precede C auto HOLD/CLOSE\n";

    // WebSocket survives Radar reconnect; B/C history belongs to each TCP session.
    const bool initialBackground = signal && signal->usesBackground();
    int result = 1;
    try
    {
        RadarSignals signals(reconnectEnabled || emulatorDoorSim);
        elevsafe::RadarReconnectBudget retryBudget;
        // Accessed only by the Radar owner thread, including its synchronous callbacks.
        bool arrivalPending = emulatorAutoCycle;
        std::optional<elevsafe::sp::DoorState> reconnectDoorState;
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
                    if (reconnectDoorState) signal->seedDoorState(*reconnectDoorState);
                    std::cerr << "SignalPipeline ready: config=" << *signalConfigPath
                              << " background=" << (signal->usesBackground() ? "enabled" : "disabled") << '\n';
                }
                if (abProfileEnabled) abProfile = std::make_unique<elevsafe::AbProfile>(profileLimit, profileWarmup);
            }
            const auto sessionStarted = std::chrono::steady_clock::now();
            if (!benchmarkMode)
                std::osyncstream(std::cerr) << "[radar] session=" << session << " begin mode="
                    << (emulatorDoorSim ? "emulator" : "radar") << " reconnect=" << reconnectEnabled
                    << " idle_timeout_ms=" << (radarIdleTimeout ? radarIdleTimeout->count() : 0) << '\n';
            // Capacity 1 is only the minimum needed for synchronous enqueue -> immediate drain.
            // It is not a production tuning value and must be revisited for a separated producer/consumer design.
            constexpr std::size_t kSynchronousFrameBufferCapacity = 1;
            elevsafe::BackendPipeline pipeline(kSynchronousFrameBufferCapacity);

            elevsafe::RetinaClient client(host, port);
            client.setProcessingMetricsEnabled(benchmarkMode);
            client.setProcessingMetricsFrameLimit(benchmarkFrameLimit);

            std::unique_ptr<elevsafe::TrackingDemo> tracking;
            if (websocket && signal && !benchmarkMode)
                tracking = std::make_unique<elevsafe::TrackingDemo>();
            std::unique_ptr<elevsafe::PeriodicRadarPublisher> periodic;
            if (periodicSnapshot) periodic = std::make_unique<elevsafe::PeriodicRadarPublisher>(*websocket);
            std::size_t snapshotsPublished = 0;
            // All simulation and Radar observation state belongs to the receive thread.
            elevsafe::DoorSimulation virtualDoor;
            elevsafe::EmulatorDoorArbitration doorArbitration(virtualDoor,
                emulatorAutoCycle ? &arrivalPending : nullptr);
            using Door = elevsafe::DoorSimulation;
            std::optional<std::int64_t> radarTimestamp;
            std::vector<TrackOutput> radarTracks;
            bool newRadarObservation = false;
            std::optional<Door::Time> lastRadarObservationAt;
            std::optional<Door::Command> pendingDoorCommand;
            bool lastSnapshotHadCommand = false;
            std::optional<Door::Clock::time_point> lastPublish;
            auto lastPublishedState = virtualDoor.state();
            using DoorCommand = elevsafe::DoorCommand;
            auto lastCRequest = DoorCommand::NONE;
            const auto serviceDoor = [&](Door::Time now)
            {
                // The command window is closed until the first valid observation.
                if (!radarTimestamp) return false;
                if (remoteControl)
                {
                    doorArbitration.advanceMotion(now);
                    constexpr auto maximumObservationAge = std::chrono::seconds(1);
                    const bool fresh = lastRadarObservationAt && now - *lastRadarObservationAt <= maximumObservationAge;
                    websocket->updateRemoteDoor(true, !fresh ? "RADAR_STALE" :
                        virtualDoor.state() == Door::State::Unknown ? "DOOR_UNKNOWN" : "");
                }
                const bool manualConsumed = serviceManualDoor(*websocket, virtualDoor,
                    wsConfig.maxCommands, session, [&](auto command) { return doorArbitration.applyManual(command, now); });
                const auto before = virtualDoor.state();
                doorArbitration.advanceMotion(now);
                if (virtualDoor.state() != before)
                    std::osyncstream(std::cerr) << "[door-sim] motion-complete session=" << session
                        << " state=" << Door::name(virtualDoor.state()) << " mode="
                        << (virtualDoor.mode() == Door::Mode::Auto ? "AUTO" : "MANUAL") << '\n';
                return manualConsumed;
            };
            const auto tickDoor = [&]
            {
                if (!radarTimestamp) return;
                const auto now = Door::Clock::now();
                serviceDoor(now); // With no fresh frame, only manual requests and ongoing motion are serviced.
                const auto applied = virtualDoor.takeAppliedCommand();
                if (periodic)
                {
                    if (applied || lastPublishedState != virtualDoor.state())
                    {
                        periodic->updateDoor(Door::name(virtualDoor.state()),
                            applied ? std::optional{Door::name(*applied)} : std::nullopt);
                        lastPublishedState = virtualDoor.state();
                    }
                    return;
                }
                if (applied) pendingDoorCommand = applied;
                if (lastPublish && now - *lastPublish < elevsafe::TrackingDemo::publishPeriod) return;
                if (lastPublish && !newRadarObservation && !pendingDoorCommand && !lastSnapshotHadCommand
                    && lastPublishedState == virtualDoor.state()) return;
                const auto snapshot = elevsafe::trackingSnapshot(*radarTimestamp, radarTracks,
                    std::chrono::system_clock::now(), Door::name(virtualDoor.state()),
                    pendingDoorCommand ? std::optional{Door::name(*pendingDoorCommand)} : std::nullopt);
                if (!websocket->publishSnapshot(snapshot)) throw std::runtime_error("emulator door snapshot publish failed");
                ++snapshotsPublished;
                lastPublish = now;
                lastPublishedState = virtualDoor.state();
                lastSnapshotHadCommand = pendingDoorCommand.has_value();
                pendingDoorCommand.reset();
                newRadarObservation = false;
            };
            bool summaryPrinted = false;
            std::optional<std::uint64_t> firstTimestamp;
            std::optional<elevsafe::sp::DoorState> lastDoorState;
            client.setFrameCallback([&pipeline, &client, &signal, &abProfile, benchmarkMode, quietFrames,
                &summaryPrinted, &lastRadarObservationAt, reconnectEnabled, &firstTimestamp, &lastDoorState, &tracking, &websocket, &snapshotsPublished, &periodic,
                emulatorDoorSim, &radarTimestamp, &radarTracks, &newRadarObservation, &serviceDoor, &doorArbitration, &virtualDoor, session, &lastCRequest](const elevsafe::RadarFrame& frame)
            {
                if (!benchmarkMode && !firstTimestamp) firstTimestamp = frame.timestampUs;
                using Profile = elevsafe::AbProfile;
                std::optional<Profile::FrameScope> profiled;
                if (abProfile)
                    profiled.emplace(*abProfile, frame.frameCount, frame.points.size(), pipeline.pendingSize() >= kSynchronousFrameBufferCapacity);
                const auto receivedAt = emulatorDoorSim ? Door::Clock::now() : Door::Time{};
                const auto pipelineStart = profiled ? Profile::Clock::now() : Profile::Clock::time_point{};
                pipeline.onFrame(frame);
                pipeline.drain([&signal, &profiled, quietFrames, reconnectEnabled, &lastDoorState, &tracking, &websocket, &snapshotsPublished, &periodic,
                emulatorDoorSim, &radarTimestamp, &radarTracks, &newRadarObservation, &serviceDoor, &doorArbitration, &virtualDoor,
                receivedAt, session, &lastCRequest, &lastRadarObservationAt](const elevsafe::RadarFrame& pendingFrame)
                {
                    if (signal)
                    {
                        try
                        {
                            const bool manualConsumed = emulatorDoorSim && serviceDoor(Door::Clock::now());
                            const auto cDoorState = emulatorDoorSim
                                ? doorArbitration.trackingDoorState() : elevsafe::DoorState::UNKNOWN;
                            const auto adapterStart = profiled ? Profile::Clock::now() : Profile::Clock::time_point{};
                            const auto input = elevsafe::toSignalFrame(pendingFrame);
                            if (profiled) profiled->sample.adapter = Profile::Clock::now() - adapterStart;
                            const elevsafe::sp::FrameContext context;
                            const auto bStart = profiled ? Profile::Clock::now() : Profile::Clock::time_point{};
                            const auto result = signal->process(input, context);
                            lastDoorState = result.estimated_door_state;
                            if (profiled)
                            {
                                const auto bEnd = Profile::Clock::now();
                                profiled->sample.b = bEnd - bStart;
                                profiled->sample.ab = bEnd - adapterStart;
                                profiled->sample.success = true;
                            }
                            if (emulatorDoorSim)
                            {
                                auto observation = tracking->observe(result, cDoorState);
                                const auto request = observation.door_command;
                                if (request != lastCRequest && request != DoorCommand::NONE)
                                    std::osyncstream(std::cerr) << "[door-sim] C-request session=" << session << " command="
                                        << (request == DoorCommand::HOLD ? "HOLD" : request == DoorCommand::CLOSE ? "CLOSE" : "OPEN") << '\n';
                                lastCRequest = request;
                                if (doorArbitration.applyFrame(request, result.timestamp_us,
                                    receivedAt, Door::Clock::now(), manualConsumed, cDoorState))
                                {
                                    if (const auto applied = virtualDoor.activeCommand())
                                        std::osyncstream(std::cerr) << "[door-sim] auto-applied session=" << session
                                            << " source=" << (request == DoorCommand::NONE ? "normal-cycle" : "C")
                                            << " command=" << Door::name(*applied) << '\n';
                                }
                                if (periodic)
                                {
                                    const auto applied = virtualDoor.takeAppliedCommand();
                                    periodic->observe(result.timestamp_us, std::move(observation.tracks),
                                        Door::name(virtualDoor.state()),
                                        applied ? std::optional{Door::name(*applied)} : std::nullopt);
                                }
                                else radarTracks = std::move(observation.tracks);
                                const bool firstObservation = !radarTimestamp;
                                radarTimestamp = result.timestamp_us;
                                lastRadarObservationAt = receivedAt;
                                newRadarObservation = true;
                                if (firstObservation) websocket->setManualCommandsEnabled(true);
                            }
                            else if (periodic)
                            {
                                auto observation = tracking->observe(result);
                                periodic->observe(result.timestamp_us, std::move(observation.tracks),
                                    elevsafe::sp::doorStateToString(result.estimated_door_state));
                            }
                            else if (tracking)
                            {
                                const auto snapshot = tracking->process(result, elevsafe::TrackingDemo::Clock::now(),
                                    std::chrono::system_clock::now());
                                if (snapshot)
                                {
                                    if (!websocket->publishSnapshot(*snapshot))
                                        throw std::runtime_error("tracking snapshot publish failed");
                                    ++snapshotsPublished;
                                }
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
                            if (periodic) periodic->checkFailure();
                            // drain already owns/popped this frame; skip it without retrying B's stateful process.
                            std::cerr << "Signal frame failed: frameCount=" << pendingFrame.frameCount
                                      << " timestampUs=" << pendingFrame.timestampUs
                                      << ": " << exception.what() << '\n';
                            if (reconnectEnabled || emulatorDoorSim) throw; // Processing errors never trigger transport recovery.
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
            std::exception_ptr receiveFailure, publisherFailure;
            const auto tick = [&]
            {
                if (periodic) periodic->checkFailure();
                if (emulatorDoorSim) tickDoor();
            };
            std::optional<std::size_t> sealedEnqueues;
            const auto sealPublisher = [&]
            {
                if (!sealedEnqueues) sealedEnqueues = periodic->seal();
            };
            try
            {
                if (reconnectEnabled || emulatorDoorSim)
                {
                    elevsafe::RetinaClient::SessionControl control;
                    if (reconnectEnabled) control.connectNotAfter = retryBudget.deadline();
                    control.receiveIdleTimeout = radarIdleTimeout;
                    if (periodic || emulatorDoorSim) control.onReceiveTick = tick;
                    if (periodic || remoteControl) control.onReceiveEnd = [&]
                    {
                        if (remoteControl) websocket->updateRemoteDoor(false, "BACKEND_UNAVAILABLE");
                        if (periodic) sealPublisher();
                    };
                    control.cancelRequested = [] { return radarStop.load(std::memory_order_relaxed); };
                    result = client.runSession(std::cerr, control, profileDiagnostics);
                }
                else if (periodic) result = client.runWithReceiveTick(std::cerr, profileDiagnostics, tick, sealPublisher);
                else result = client.run(std::cerr, {}, profileDiagnostics);
                runReturned = true;
            }
            catch (...) { receiveFailure = std::current_exception(); }

            const auto reportFailure = [](const char* label, const std::exception_ptr& failure)
            {
                try { std::rethrow_exception(failure); }
                catch (const std::exception& error) { std::osyncstream(std::cerr) << label << error.what() << '\n'; }
                catch (...) { std::osyncstream(std::cerr) << label << "unknown exception\n"; }
            };

            if (periodic)
            {
                try
                {
                    sealPublisher();
                    const auto stats = periodic->finish();
                    snapshotsPublished = stats.published;
                    std::cerr << "[ws-periodic] session=" << session << " published=" << stats.published
                              << " mean_interval_us=" << stats.meanIntervalUs
                              << " sealed_enqueues=" << *sealedEnqueues << '\n';
                }
                catch (...)
                {
                    publisherFailure = std::current_exception();
                    result = 1;
                    std::osyncstream(std::cerr) << "[ws-periodic] session=" << session
                        << " joined transport_stage=" << profileDiagnostics.stage << '\n';
                    profileDiagnostics.stage = "publisher_failure";
                }
            }
            if (emulatorDoorSim)
            {
                if (remoteControl) websocket->updateRemoteDoor(false, "BACKEND_UNAVAILABLE");
                websocket->setManualCommandsEnabled(false);
            }
            if (publisherFailure) reportFailure("[ws-periodic] publish failed: ", publisherFailure);
            if (receiveFailure && receiveFailure != publisherFailure)
            {
                if (!publisherFailure) profileDiagnostics.stage = "processing_exception";
                reportFailure("Backend stopped: ", receiveFailure);
            }

            if (tracking)
                std::cerr << "[tracking-demo] session=" << session << " frames=" << tracking->processedFrames()
                          << " snapshots=" << snapshotsPublished << " exit=" << result << '\n';
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
                          << profileDiagnostics.stage
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

            if (!benchmarkMode)
            {
                std::osyncstream sessionLog(std::cerr);
                sessionLog << "[radar] session=" << session << " end stage=" << profileDiagnostics.stage
                          << " code=" << profileDiagnostics.errorCode
                          << " connected=" << profileDiagnostics.connected
                          << " parsed=" << profileDiagnostics.parser.packetsParsed
                          << " rejected=" << profileDiagnostics.parser.packetsRejected
                          << " buffered=" << profileDiagnostics.bufferedBytes;
                if (signal) sessionLog << " B_frames=" << signal->stats().frames
                                      << " background=" << signal->usesBackground()
                                      << " last_door_state=" << elevsafe::sp::doorStateToString(
                                             lastDoorState.value_or(elevsafe::sp::DoorState::UNKNOWN));
                if (firstTimestamp) sessionLog << " first_timestamp_us=" << *firstTimestamp;
                if (profileDiagnostics.connected)
                {
                    const auto age = [](const std::optional<std::chrono::milliseconds>& value)
                    { return value ? std::to_string(value->count()) : std::string("never"); };
                    sessionLog << " first_byte_pending=" << !profileDiagnostics.lastByteAge.has_value()
                              << " last_byte_age_ms=" << age(profileDiagnostics.lastByteAge)
                              << " last_valid_frame_age_ms=" << age(profileDiagnostics.lastValidFrameAge);
                }
                if (!profileDiagnostics.socketOperation.empty())
                    sessionLog << " socket_operation=" << profileDiagnostics.socketOperation;
                sessionLog << " snapshots=" << snapshotsPublished << " exit_code=" << result
                    << " duration_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - sessionStarted).count() << '\n';
            }
            if (!reconnectEnabled || !runReturned || publisherFailure) break;
            if (radarStop.load(std::memory_order_relaxed) || profileDiagnostics.cancelled)
            { result = 0; break; }
            if (!elevsafe::RetinaClient::retryable(profileDiagnostics, reconnectEof)) break;
            const auto next = retryBudget.schedule(elevsafe::RadarReconnectBudget::Clock::now(), profileDiagnostics.connected);
            if (!next)
            {
                std::cerr << "[radar] recovery exhausted retries=" << retryBudget.retries() << " (limit=3, budget=15s)\n";
                result = 1; break;
            }
            if (lastDoorState) reconnectDoorState = lastDoorState;
            std::osyncstream(std::cerr) << "[radar] retry=" << retryBudget.retries()
                << "/3 wait_ms=1000 connect_timeout_ms=3000 after_session=" << session
                << " reason=" << profileDiagnostics.stage << " code=" << profileDiagnostics.errorCode << '\n';
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
