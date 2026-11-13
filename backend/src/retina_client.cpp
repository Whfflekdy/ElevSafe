#include "elevsafe/retina_client.h"

#include "elevsafe/relative_frame_clock.h"
#include "elevsafe/retina_protocol.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
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
            bool start(std::ostream& log, int& errorCode)
            {
#ifdef _WIN32
                WSADATA data{};
                const int result = WSAStartup(MAKEWORD(2, 2), &data);
                if (result != 0)
                {
                    errorCode = result;
                    log << "WSAStartup failed: " << result << '\n';
                    return false;
                }
                m_started = true;
#else
                (void)log;
                (void)errorCode;
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

        int lastSocketError()
        {
#ifdef _WIN32
            return WSAGetLastError();
#else
            return errno;
#endif
        }

        bool pendingSocketError(int error)
        {
#ifdef _WIN32
            return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
            return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS;
#endif
        }

        int timeoutError()
        {
#ifdef _WIN32
            return WSAETIMEDOUT;
#else
            return ETIMEDOUT;
#endif
        }

        int cancelledError()
        {
#ifdef _WIN32
            return WSAEINTR;
#else
            return ECANCELED;
#endif
        }

        int makeNonblocking(std::intptr_t socket)
        {
#ifdef _WIN32
            u_long mode = 1;
            return ioctlsocket(static_cast<SOCKET>(socket), FIONBIO, &mode) == 0 ? 0 : lastSocketError();
#else
            const int flags = fcntl(static_cast<int>(socket), F_GETFL, 0);
            return flags >= 0 && fcntl(static_cast<int>(socket), F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : errno;
#endif
        }

        int waitSocket(std::intptr_t socket, bool writing, const RetinaClient::Deadline& deadline,
            const std::function<bool()>& cancelled = {})
        {
            for (;;)
            {
                if (cancelled && cancelled()) return cancelledError();
                const auto left = deadline ? deadline() - std::chrono::steady_clock::now() : std::chrono::seconds(1);
                if (left <= std::chrono::steady_clock::duration::zero()) return timeoutError();
                // Re-query the same-thread deadline periodically; no watchdog thread.
                const auto micros = std::max<std::int64_t>(1, std::min<std::int64_t>(cancelled ? 100000 : 1000000,
                    std::chrono::duration_cast<std::chrono::microseconds>(left).count()));
                timeval interval{static_cast<long>(micros / 1000000), static_cast<long>(micros % 1000000)};
                fd_set ready, errors;
                FD_ZERO(&ready); FD_ZERO(&errors);
#ifdef _WIN32
                const auto native = static_cast<SOCKET>(socket);
                const int width = 0;
#else
                const auto native = static_cast<int>(socket);
                if (native >= FD_SETSIZE) return EINVAL;
                const int width = native + 1;
#endif
                FD_SET(native, &ready); FD_SET(native, &errors);
                const int result = select(width, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &interval);
                if (result > 0) return 0;
                if (result == 0) continue;
                const int error = lastSocketError();
#ifdef _WIN32
                if (error == WSAEINTR) continue;
#else
                if (error == EINTR) continue;
#endif
                return error;
            }
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
        RunDiagnostics diagnostics;
        return run(log, {}, diagnostics);
    }

    int RetinaClient::run(std::ostream& log, const Deadline& deadline, RunDiagnostics& diagnostics)
    {
        return runImpl(log, deadline, nullptr, diagnostics);
    }

    int RetinaClient::runSession(std::ostream& log, const SessionControl& control, RunDiagnostics& diagnostics)
    {
        if (control.connectTimeout <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("connectTimeout must be positive");
        return runImpl(log, {}, &control, diagnostics);
    }

    bool RetinaClient::retryable(const RunDiagnostics& diagnostics, bool reconnectEof) noexcept
    {
        if (diagnostics.cancelled) return false;
        if (diagnostics.stage == "eof") return reconnectEof;
        if (diagnostics.stage != "connect" && diagnostics.stage != "receive") return false;
#ifdef _WIN32
        return diagnostics.errorCode == WSAECONNREFUSED || diagnostics.errorCode == WSAECONNRESET
            || diagnostics.errorCode == WSAECONNABORTED || diagnostics.errorCode == WSAETIMEDOUT
            || diagnostics.errorCode == WSAENETUNREACH || diagnostics.errorCode == WSAEHOSTUNREACH
            || diagnostics.errorCode == WSAENETDOWN || diagnostics.errorCode == WSAENETRESET;
#else
        return diagnostics.errorCode == ECONNREFUSED || diagnostics.errorCode == ECONNRESET
            || diagnostics.errorCode == ECONNABORTED || diagnostics.errorCode == ETIMEDOUT
            || diagnostics.errorCode == ENETUNREACH || diagnostics.errorCode == EHOSTUNREACH
            || diagnostics.errorCode == ENETDOWN || diagnostics.errorCode == ENETRESET;
#endif
    }

    int RetinaClient::runImpl(std::ostream& log, const Deadline& deadline,
        const SessionControl* control, RunDiagnostics& diagnostics)
    {
        diagnostics = {};
        const auto connectEnd = control ? (std::min)(std::chrono::steady_clock::now() + control->connectTimeout,
            control->connectNotAfter.value_or((std::chrono::steady_clock::time_point::max)()))
            : (std::chrono::steady_clock::time_point::max)();
        const Deadline connectDeadline = control ? Deadline([connectEnd] { return connectEnd; }) : deadline;
        const std::function<bool()> cancelled = [this, control]
        { return m_stopRequested || (control && control->cancelRequested && control->cancelRequested()); };
        m_stopRequested = false;
        if (control && cancelled())
        {
            diagnostics.stage = "stopped"; diagnostics.cancelled = true;
            log << "TCP cancelled before connection\n";
            return 0;
        }
        if (connectDeadline && std::chrono::steady_clock::now() >= connectDeadline())
        {
            diagnostics.stage = "start";
            diagnostics.errorCode = timeoutError();
            diagnostics.timedOut = true;
            log << "TCP deadline expired before connection: " << socketError(diagnostics.errorCode) << '\n';
            return 1;
        }
        m_stopRequested = false;
        m_processingSummary = {};
        m_processingMetricsFrozen = false;
        closeSocket();

        SocketRuntime runtime;
        diagnostics.stage = "socket_runtime";
        if (!runtime.start(log, diagnostics.errorCode))
            return 1;
        // Close before WSACleanup, including callback exceptions and setup failures.
        const auto cleanup = std::unique_ptr<RetinaClient, void(*)(RetinaClient*)>(this,
            [](RetinaClient* client) { client->closeSocket(); });

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (deadline || control) hints.ai_flags = AI_NUMERICHOST; // No DNS in bounded modes.

        addrinfo* addresses = nullptr;
        const std::string port = std::to_string(m_port);
        const int resolveResult = getaddrinfo(m_host.c_str(), port.c_str(), &hints, &addresses);
        if (resolveResult != 0)
        {
            diagnostics.stage = "resolve";
            diagnostics.errorCode = resolveResult;
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
            if (control && cancelled()) break;
            if (connectDeadline && std::chrono::steady_clock::now() >= connectDeadline())
            { lastConnectError = timeoutError(); haveConnectError = true; break; }
#ifdef _WIN32
            const SOCKET socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket == INVALID_SOCKET)
            {
                lastConnectError = lastSocketError(); haveConnectError = true;
                continue;
            }
            m_socket = static_cast<std::intptr_t>(socket);
#else
            const int socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket < 0)
            {
                lastConnectError = lastSocketError(); haveConnectError = true;
                continue;
            }
            m_socket = socket;
#endif

            const int setupError = (deadline || control) ? makeNonblocking(m_socket) : 0;
            const int connectResult = setupError == 0 ? ::connect(
#ifdef _WIN32
                static_cast<SOCKET>(m_socket),
#else
                static_cast<int>(m_socket),
#endif
                address->ai_addr,
                static_cast<socklen_t>(address->ai_addrlen)) : -1;
            int connectError = setupError != 0 ? setupError : (connectResult == 0 ? 0 : lastSocketError());
            if (connectDeadline && pendingSocketError(connectError))
            {
                connectError = waitSocket(m_socket, true, connectDeadline, control ? cancelled : std::function<bool()>{});
                if (connectError == 0)
                {
                    int socketErrorCode = 0;
                    socklen_t length = sizeof(socketErrorCode);
                    if (getsockopt(
#ifdef _WIN32
                        static_cast<SOCKET>(m_socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketErrorCode),
#else
                        static_cast<int>(m_socket), SOL_SOCKET, SO_ERROR, &socketErrorCode,
#endif
                        &length) != 0) connectError = lastSocketError();
                    else connectError = socketErrorCode;
                }
            }
            if (connectError == 0)
            {
                if (control && std::chrono::steady_clock::now() >= connectEnd)
                { lastConnectError = timeoutError(); haveConnectError = true; closeSocket(); break; }
                break;
            }

            lastConnectError = connectError;
            haveConnectError = true;
            closeSocket();
            // Never hide WSAEFAULT by trying another resolved address.
            if ((connectDeadline && connectError == timeoutError()) || (control && cancelled())) break;
#ifdef _WIN32
            if (control && connectError == WSAEFAULT) break;
#endif
        }

        freeaddrinfo(addresses);

        if (control && cancelled())
        {
            diagnostics.stage = "stopped"; diagnostics.cancelled = true;
            log << "TCP cancelled during connection\n";
            return 0;
        }

        if (isInvalidSocket(m_socket))
        {
            diagnostics.stage = "connect";
            diagnostics.errorCode = haveConnectError ? lastConnectError : lastSocketError();
            diagnostics.timedOut = diagnostics.errorCode == timeoutError();
            const std::string error = haveConnectError ? socketError(lastConnectError) : currentSocketError();
            log << "TCP connect failed for " << m_host << ':' << m_port << ": " << error << '\n';
            return 1;
        }

        log << "Connected to " << m_host << ':' << m_port << '\n';
        diagnostics.connected = true;
        diagnostics.stage = "receiving";

        RelativeFrameClock frameClock;
        RetinaStreamParser parser;
        parser.setProcessingMetricsEnabled(m_processingMetricsEnabled);
        if (m_processingMetricsFrameLimit.has_value())
            parser.setProcessingMetricsFrameLimit(*m_processingMetricsFrameLimit);
        parser.setFrameCallback([this, &frameClock, &parser, &cancelled, control](RadarFrame& frame)
        {
            if (m_stopRequested || (control && cancelled())) { m_stopRequested = true; return; }
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
            if (deadline || control)
            {
                const int error = waitSocket(m_socket, false, deadline, control ? cancelled : std::function<bool()>{});
                if (error != 0)
                {
                    if (control && cancelled())
                    {
                        diagnostics.stage = "stopped"; diagnostics.cancelled = true;
                        log << "TCP cancelled while receiving\n";
                        break;
                    }
                    diagnostics.stage = "receive";
                    diagnostics.errorCode = error;
                    diagnostics.timedOut = error == timeoutError();
                    log << "TCP receive wait failed: " << socketError(error) << '\n';
                    result = 1;
                    break;
                }
            }
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
                diagnostics.stage = "eof";
                log << "Remote endpoint closed the connection.\n";
                break;
            }

            if (received < 0)
            {
                const int error = lastSocketError();
                if ((deadline || control) && pendingSocketError(error)) continue;
                if (!m_stopRequested)
                {
                    diagnostics.stage = "receive";
                    diagnostics.errorCode = error;
                    log << "TCP receive failed: " << socketError(error) << '\n';
                    result = 1;
                }
                break;
            }

            parser.feed(std::span<const std::uint8_t>(
                receiveBuffer.data(),
                static_cast<std::size_t>(received)));
        }

        m_processingSummary = parser.processingSummary();
        diagnostics.parser = parser.statistics();
        diagnostics.bufferedBytes = parser.bufferedBytes();
        if (m_stopRequested && diagnostics.stage == "receiving") diagnostics.stage = "stopped";
        if (control && cancelled()) { diagnostics.stage = "stopped"; diagnostics.cancelled = true; }
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
