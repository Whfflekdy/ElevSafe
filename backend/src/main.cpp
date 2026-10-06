#include "elevsafe/backend_pipeline.h"
#include "elevsafe/retina_client.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

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
}

int main(int argc, char** argv)
{
    const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    std::uint16_t port = 29172;

    if (argc >= 3 && !parsePort(argv[2], port))
    {
        std::cerr << "Invalid port '" << argv[2] << "'. Port must be in the range 1-65535.\n"
                  << "Usage: elevsafe_backend [host] [port]\n";
        return 2;
    }

    // Capacity 1 is only the minimum needed for synchronous enqueue -> immediate drain.
    // It is not a production tuning value and must be revisited for a separated producer/consumer design.
    constexpr std::size_t kSynchronousFrameBufferCapacity = 1;
    elevsafe::BackendPipeline pipeline(kSynchronousFrameBufferCapacity);

    elevsafe::RetinaClient client(host, port, [&pipeline](const elevsafe::RadarFrame& frame)
    {
        pipeline.onFrame(frame);
        pipeline.drain([](const elevsafe::RadarFrame& pendingFrame)
        {
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
    });

    std::cerr << "Connecting to " << host << ':' << port << "...\n";
    return client.run(std::cerr);
}
