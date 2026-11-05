#include "elevsafe/backend_pipeline.h"
#include "elevsafe/retina_client.h"
#include "elevsafe/signal_adapter.h"
#include "pipeline/signal_pipeline.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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
        std::cerr << "Usage: elevsafe_backend [host] [port] [--benchmark] [--benchmark-frames N]"
                     " [--signal-config PATH]\n";
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
}

int main(int argc, char** argv)
{
    const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    std::uint16_t port = 29172;

    if (argc >= 3 && !parsePort(argv[2], port))
    {
        std::cerr << "Invalid port '" << argv[2] << "'. Port must be in the range 1-65535.\n";
        printUsage();
        return 2;
    }

    bool benchmarkMode = false;
    std::optional<std::size_t> benchmarkFrameLimit;
    std::optional<std::string> signalConfigPath;
    for (int argument = 3; argument < argc;)
    {
        const std::string_view option(argv[argument]);
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

        std::cerr << "Unknown option '" << option << "'.\n";
        printUsage();
        return 2;
    }

    if (benchmarkFrameLimit.has_value() && !benchmarkMode)
    {
        std::cerr << "--benchmark-frames requires --benchmark.\n";
        printUsage();
        return 2;
    }

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
            signal = std::make_unique<elevsafe::sp::SignalPipeline>(config);
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

    // Capacity 1 is only the minimum needed for synchronous enqueue -> immediate drain.
    // It is not a production tuning value and must be revisited for a separated producer/consumer design.
    constexpr std::size_t kSynchronousFrameBufferCapacity = 1;
    elevsafe::BackendPipeline pipeline(kSynchronousFrameBufferCapacity);

    elevsafe::RetinaClient client(host, port);
    client.setProcessingMetricsEnabled(benchmarkMode);
    client.setProcessingMetricsFrameLimit(benchmarkFrameLimit);

    bool summaryPrinted = false;
    client.setFrameCallback([&pipeline, &client, &signal, benchmarkMode, &summaryPrinted](const elevsafe::RadarFrame& frame)
    {
        pipeline.onFrame(frame);
        pipeline.drain([&signal, benchmarkMode](const elevsafe::RadarFrame& pendingFrame)
        {
            if (signal)
            {
                try
                {
                    const auto input = elevsafe::toSignalFrame(pendingFrame);
                    const elevsafe::sp::FrameContext context;
                    const auto result = signal->process(input, context);
                    if (!benchmarkMode)
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
                }
            }

            if (benchmarkMode)
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

        if (benchmarkMode && !summaryPrinted && client.processingMetricsFrozen())
        {
            printProcessingSummary(client.processingSummary());
            summaryPrinted = true;
        }
    });

    std::cerr << "Connecting to " << host << ':' << port << "...\n";
    int result = 1;
    try
    {
        result = client.run(std::cerr);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Backend stopped: " << exception.what() << '\n';
    }

    if (benchmarkMode && !summaryPrinted)
        printProcessingSummary(client.processingSummary());

    return result;
}
