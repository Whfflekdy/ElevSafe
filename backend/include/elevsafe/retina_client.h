#pragma once

#include "radar_types.h"

#include <cstdint>
#include <functional>
#include <iosfwd>
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

        // Blocking receive loop. Returns 0 on a clean disconnect, otherwise 1.
        int run(std::ostream& log);
        void stop();

    private:
        void closeSocket();

        std::string m_host;
        std::uint16_t m_port;
        FrameCallback m_callback;
        std::intptr_t m_socket = -1;
        bool m_stopRequested = false;
    };
}
