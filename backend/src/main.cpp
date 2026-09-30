#include "elevsafe/retina_client.h"

#include <charconv>
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

    elevsafe::RetinaClient client(host, port, [](const elevsafe::RadarFrame& frame)
    {
        std::cout << "frameCount=" << frame.frameCount
                  << " pointCount=" << frame.points.size();

        if (!frame.points.empty())
        {
            const auto& point = frame.points.front();
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

    std::cerr << "Connecting to " << host << ':' << port << "...\n";
    return client.run(std::cerr);
}
