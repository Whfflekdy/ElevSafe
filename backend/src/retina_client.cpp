#include "elevsafe/retina_client.h"

#include "elevsafe/relative_frame_clock.h"
#include "elevsafe/retina_protocol.h"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace elevsafe
{
    namespace
    {
        class SocketRuntime
        {
        public:
            bool start(std::ostream& log)
            {
#ifdef _WIN32
                WSADATA data{};
                const int result = WSAStartup(MAKEWORD(2, 2), &data);
                if (result != 0)
                {
                    log << "WSAStartup failed: " << result << '\n';
                    return false;
                }
                m_started = true;
#else
                (void)log;
#endif
                return true;
            }

            ~SocketRuntime()
            {
#ifdef _WIN32
                if (m_started)
                    WSACleanup();
#endif
            }

        private:
#ifdef _WIN32
            bool m_started = false;
#endif
        };

        std::string socketError(int errorCode)
        {
#ifdef _WIN32
            return "winsock error " + std::to_string(errorCode);
#else
            return std::strerror(errorCode);
#endif
        }

        std::string currentSocketError()
        {
#ifdef _WIN32
            return socketError(WSAGetLastError());
#else
            return socketError(errno);
#endif
        }

        bool isInvalidSocket(std::intptr_t socket)
        {
#ifdef _WIN32
            return static_cast<SOCKET>(socket) == INVALID_SOCKET;
#else
            return socket < 0;
#endif
        }
    }

    RetinaClient::RetinaClient(std::string host, std::uint16_t port, FrameCallback callback) :
        m_host(std::move(host)),
        m_port(port),
        m_callback(std::move(callback))
    {
    }

    RetinaClient::~RetinaClient()
    {
        closeSocket();
    }

    void RetinaClient::setFrameCallback(FrameCallback callback)
    {
        m_callback = std::move(callback);
    }

    void RetinaClient::setProcessingMetricsEnabled(bool enabled) noexcept
    {
        m_processingMetricsEnabled = enabled;
    }

    void RetinaClient::setProcessingMetricsFrameLimit(std::optional<std::size_t> frameLimit) noexcept
    {
        m_processingMetricsFrameLimit = frameLimit;
    }

    const ParserProcessingSummary& RetinaClient::processingSummary() const noexcept
    {
        return m_processingSummary;
    }

    bool RetinaClient::processingMetricsFrozen() const noexcept
    {
        return m_processingMetricsFrozen;
    }

    int RetinaClient::run(std::ostream& log)
    {
        m_stopRequested = false;
        m_processingSummary = {};
        m_processingMetricsFrozen = false;
        closeSocket();

        SocketRuntime runtime;
        if (!runtime.start(log))
            return 1;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* addresses = nullptr;
        const std::string port = std::to_string(m_port);
        const int resolveResult = getaddrinfo(m_host.c_str(), port.c_str(), &hints, &addresses);
        if (resolveResult != 0)
        {
#ifdef _WIN32
            log << "getaddrinfo failed: " << resolveResult << '\n';
#else
            log << "getaddrinfo failed: " << gai_strerror(resolveResult) << '\n';
#endif
            return 1;
        }

        int lastConnectError = 0;
        bool haveConnectError = false;

        for (addrinfo* address = addresses; address != nullptr; address = address->ai_next)
        {
#ifdef _WIN32
            const SOCKET socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket == INVALID_SOCKET)
                continue;
            m_socket = static_cast<std::intptr_t>(socket);
#else
            const int socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket < 0)
                continue;
            m_socket = socket;
#endif

            if (::connect(
#ifdef _WIN32
                static_cast<SOCKET>(m_socket),
#else
                static_cast<int>(m_socket),
#endif
                address->ai_addr,
                static_cast<socklen_t>(address->ai_addrlen)) == 0)
            {
                break;
            }

            #ifdef _WIN32
            lastConnectError = WSAGetLastError();
            #else
            lastConnectError = errno;
            #endif
            haveConnectError = true;
            closeSocket();
        }

        freeaddrinfo(addresses);

        if (isInvalidSocket(m_socket))
        {
            const std::string error = haveConnectError ? socketError(lastConnectError) : currentSocketError();
            log << "TCP connect failed for " << m_host << ':' << m_port << ": " << error << '\n';
            return 1;
        }

        log << "Connected to " << m_host << ':' << m_port << '\n';

        RelativeFrameClock frameClock;
        RetinaStreamParser parser;
        parser.setProcessingMetricsEnabled(m_processingMetricsEnabled);
        if (m_processingMetricsFrameLimit.has_value())
            parser.setProcessingMetricsFrameLimit(*m_processingMetricsFrameLimit);
        parser.setFrameCallback([this, &frameClock, &parser](RadarFrame& frame)
        {
            frame.timestampUs = frameClock.timestampUs(RelativeFrameClock::Clock::now());
            if (!m_processingMetricsFrozen && parser.processingMetricsFrozen())
            {
                m_processingSummary = parser.processingSummary();
                m_processingMetricsFrozen = true;
            }
            if (m_callback)
                m_callback(frame);
        });

        std::array<std::uint8_t, 8192> receiveBuffer{};
        int result = 0;

        while (!m_stopRequested)
        {
#ifdef _WIN32
            const int received = ::recv(
                static_cast<SOCKET>(m_socket),
                reinterpret_cast<char*>(receiveBuffer.data()),
                static_cast<int>(receiveBuffer.size()),
                0);
#else
            const ssize_t received = ::recv(
                static_cast<int>(m_socket),
                receiveBuffer.data(),
                receiveBuffer.size(),
                0);
#endif

            if (received == 0)
            {
                log << "Remote endpoint closed the connection.\n";
                break;
            }

            if (received < 0)
            {
                if (!m_stopRequested)
                {
                    log << "TCP receive failed: " << currentSocketError() << '\n';
                    result = 1;
                }
                break;
            }

            parser.feed(std::span<const std::uint8_t>(
                receiveBuffer.data(),
                static_cast<std::size_t>(received)));
        }

        m_processingSummary = parser.processingSummary();
        m_processingMetricsFrozen = parser.processingMetricsFrozen();
        closeSocket();
        return result;
    }

    void RetinaClient::stop()
    {
        m_stopRequested = true;
        closeSocket();
    }

    void RetinaClient::closeSocket()
    {
        if (isInvalidSocket(m_socket))
        {
            m_socket = -1;
            return;
        }

#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(m_socket), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(m_socket));
#else
        ::shutdown(static_cast<int>(m_socket), SHUT_RDWR);
        ::close(static_cast<int>(m_socket));
#endif
        m_socket = -1;
    }
}
