#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace elevsafe
{
    struct WebSocketConfig
    {
        std::string address = "127.0.0.1";
        std::uint16_t port = 8080;
        std::size_t maxClients = 8;
        std::size_t maxSnapshotBytes = 65536;
        std::size_t maxRequestBytes = 4096;
        std::size_t maxCommands = 32;
    };

    struct ManualDoorRequest
    {
        std::uint64_t clientId;
        std::string command;
    };

    struct WebSocketStatistics
    {
        std::size_t connected = 0;
        std::size_t accepted = 0;
        std::size_t snapshotsSent = 0;
        std::size_t ingressReplaced = 0;
        std::size_t sessionReplaced = 0;
        std::size_t commandsReceived = 0;
        std::size_t rejectedRequests = 0;
        std::size_t commandQueueFull = 0;
        std::size_t disconnected = 0;
        std::size_t writeTimeouts = 0;
        std::size_t discardedSnapshots = 0;
        std::size_t ingressPending = 0;
        std::size_t sessionPending = 0;
        std::size_t activeWrites = 0;
        std::size_t peakIngressPending = 0;
        std::size_t peakSessionPending = 0;
        std::size_t peakActiveWrites = 0;
    };

    class WebSocketServer
    {
    public:
        explicit WebSocketServer(WebSocketConfig config, std::ostream& log);
        ~WebSocketServer();
        WebSocketServer(const WebSocketServer&) = delete;
        WebSocketServer& operator=(const WebSocketServer&) = delete;

        // start/stop belong to the owner thread. Other methods may cross threads.
        void start();
        void stop() noexcept;
        bool publishSnapshot(std::string message);
        std::optional<ManualDoorRequest> popCommand();
        // Individual atomic counters, not a transactional multi-field snapshot.
        WebSocketStatistics statistics() const noexcept;
        std::uint16_t port() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
