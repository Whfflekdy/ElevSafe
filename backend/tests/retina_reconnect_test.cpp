#include "elevsafe/retina_client.h"
#include "elevsafe/radar_reconnect.h"
#include "elevsafe/periodic_radar_publisher.h"
#include "elevsafe/websocket_server.h"
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
    void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

    std::vector<std::uint8_t> emptyFramePacket()
    {
        std::vector<std::uint8_t> packet;
        const auto append = [&](std::uint64_t word, std::size_t bytes)
        {
            for (std::size_t i = 0; i < bytes; ++i)
            {
                packet.push_back(static_cast<std::uint8_t>(word));
                word >>= 8;
            }
        };
        for (std::uint32_t word : {0u, elevsafe::kPacketMagic, 0u, 0u,
                static_cast<std::uint32_t>(elevsafe::kRadarFrameHeaderSize), 0u, 0u, 0u, 0u})
            append(word, 4);
        append(elevsafe::kFrameMagic, 8);
        append(1, 4);
        append(0, 4);
        return packet;
    }

    void testReceiveIdle()
    {
        using Client = elevsafe::RetinaClient;
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        namespace net = boost::asio;
        using net::ip::tcp;
        const auto packet = emptyFramePacket();

        for (const bool cancel : {false, true})
        {
            net::io_context io;
            tcp::acceptor listener(io, {net::ip::address_v4::loopback(), 0});
            listener.non_blocking(true);
            tcp::socket peer(io);
            std::ostringstream log;
            Client client("127.0.0.1", listener.local_endpoint().port());
            Client::SessionControl control;
            Client::RunDiagnostics status;
            control.receiveIdleTimeout = 150ms;
            bool cancelled = false;
            int frames = 0;
            int receiveEnds = 0;
            control.onReceiveEnd = [&] { ++receiveEnds; };
            client.setFrameCallback([&](const elevsafe::RadarFrame& frame)
            {
                expect(frame.points.empty() && frame.timestampUs == 0, "empty frame or session timestamp changed");
                ++frames;
            });
            control.cancelRequested = [&] { return cancelled; };
            control.onReceiveTick = [&]
            {
                if (peer.is_open()) return;
                listener.accept(peer);
                // Simulate a slow owner-thread callback. Queue data and FIN before it returns.
                std::this_thread::sleep_for(*control.receiveIdleTimeout);
                net::write(peer, net::buffer(packet));
                peer.shutdown(tcp::socket::shutdown_send);
                cancelled = cancel;
            };
            expect(client.runSession(log, control, status) == 0 && !status.timedOut && status.errorCode == 0,
                "pending bytes or cancellation lost at the idle deadline");
            expect(receiveEnds == 1, "EOF or cancellation did not seal exactly once");
            if (cancel)
                expect(status.cancelled && status.stage == "stopped" && frames == 0
                    && !status.lastByteAge && !status.lastValidFrameAge && !Client::retryable(status, true),
                    "cancellation did not take priority");
            else
                expect(status.stage == "eof" && frames == 1 && status.parser.packetsParsed == 1
                    && status.lastByteAge && status.lastValidFrameAge, "queued empty frame or FIN was lost");
        }

        net::io_context io;
        tcp::acceptor listener(io, {net::ip::address_v4::loopback(), 0});
        listener.non_blocking(true);
        tcp::socket peer(io);
        std::ostringstream log;
        Client client("127.0.0.1", listener.local_endpoint().port());
        Client::SessionControl control;
        Client::RunDiagnostics status;
        control.receiveIdleTimeout = 300ms;
        auto malformed = packet;
        malformed[elevsafe::kNetworkPacketHeaderSize] ^= 1u;
        int writes = 0;
        Clock::time_point nextSend{};
        control.onReceiveTick = [&]
        {
            if (!peer.is_open()) listener.accept(peer);
            const auto now = Clock::now();
            if (writes >= 8 || now < nextSend) return;
            if (writes == 0)
                net::write(peer, net::buffer(malformed));
            else
                net::write(peer, net::buffer(packet.data() + writes - 1, 1));
            ++writes;
            nextSend = now + 60ms;
            if (writes == 8) peer.shutdown(tcp::socket::shutdown_send);
        };
        expect(client.runSession(log, control, status) == 0 && status.stage == "eof" && !status.timedOut,
            "partial and rejected bytes must refresh the idle deadline");
        expect(writes == 8 && status.parser.packetsParsed == 0 && status.parser.packetsRejected == 1
            && status.bufferedBytes == 7 && status.lastByteAge && !status.lastValidFrameAge,
            "byte arrival and valid frame diagnostics must be independent");
        std::cout << "receive idle checks passed: expired readiness, cancellation, empty and rejected frames, partial bytes\n";
    }

    void testPeriodicPublisherFailure()
    {
        using Client = elevsafe::RetinaClient;
        using namespace std::chrono_literals;
        namespace net = boost::asio;
        using net::ip::tcp;

        const auto packet = emptyFramePacket();
        for (const bool keepSending : {false, true})
        {
            net::io_context io;
            tcp::acceptor listener(io, {net::ip::address_v4::loopback(), 0});
            listener.non_blocking(true);
            tcp::socket peer(io);
            std::ostringstream log;
            elevsafe::WebSocketConfig config;
            config.port = 0;
            elevsafe::WebSocketServer server(config, log);
            // A stopped transport fails enqueue in the idle case. A running
            // transport exercises serialization failure during active input.
            if (keepSending) server.start();
            elevsafe::PeriodicRadarPublisher publisher(server);
            Client client(keepSending ? "127.0.0.1" : "localhost", listener.local_endpoint().port());
            Client::RunDiagnostics status;
            std::size_t frames = 0;
            const std::string_view expected = keepSending ? "invalid tracking snapshot" : "periodic tracking snapshot publish failed";
            client.setFrameCallback([&](const elevsafe::RadarFrame& frame)
            {
                ++frames;
                if (!keepSending || frames >= 3)
                    publisher.observe(frame.timestampUs, {}, keepSending ? "invalid" : "CLOSED");
            });
            const auto start = std::chrono::steady_clock::now();
            const auto tick = [&]
            {
                publisher.checkFailure();
                expect(std::chrono::steady_clock::now() - start < 3s, "publisher failure did not reach the Radar owner");
                if (!peer.is_open())
                {
                    listener.accept(peer);
                    for (int i = 0; i < (keepSending ? 3 : 1); ++i)
                        net::write(peer, net::buffer(packet));
                }
                else if (keepSending && frames < 10) net::write(peer, net::buffer(packet));
            };
            const auto end = [&] { publisher.seal(); };
            bool received = false;
            try
            {
                if (keepSending)
                {
                    Client::SessionControl control;
                    control.onReceiveTick = tick;
                    control.onReceiveEnd = end;
                    client.runSession(log, control, status);
                }
                else client.runWithReceiveTick(log, status, tick, end);
            }
            catch (const std::exception& error)
            {
                expect(error.what() == expected, "original publisher error was lost");
                received = true;
            }
            expect(received && status.connected && status.errorCode == 0 && !Client::retryable(status, true)
                && (keepSending ? frames >= 3 : frames == 1), "publish failure was mistaken for a Radar socket failure");
            bool joined = false;
            try { (void)publisher.finish(); }
            catch (const std::exception& error)
            {
                expect(error.what() == expected, "join did not preserve the publisher failure");
                joined = true;
            }
            expect(joined, "publisher exception was lost");
            std::cout << "PASS publisher failure input=" << (keepSending ? "active" : "idle")
                << " radar_frames=" << frames << " native_error=" << status.errorCode << " joined=1\n";
        }

        std::ostringstream log;
        elevsafe::WebSocketConfig config;
        config.port = 0;
        elevsafe::WebSocketServer server(config, log);
        server.start();
        elevsafe::PeriodicRadarPublisher publisher(server);
        publisher.observe(0, {}, "CLOSED");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (server.statistics().peakIngressPending == 0 && std::chrono::steady_clock::now() < deadline)
        {
            publisher.checkFailure();
            std::this_thread::sleep_for(1ms);
        }
        const auto enqueues = publisher.seal();
        expect(enqueues != 0, "no snapshot was enqueued before seal");
        publisher.observe(1, {}, "OPEN");
        publisher.updateDoor("OPEN", "OPEN");
        expect(publisher.finish().published == enqueues, "snapshot enqueued after seal");
        std::cout << "PASS periodic session seal enqueued_before=" << enqueues << " enqueued_after=0\n";
    }

    // Isolated fixture process: consumes bytes produced by the existing PowerShell Make-Packet.
    // Process exit makes server shutdown/restart unambiguous, without sharing a Windows port.
    int serve(const char* port, const char* payload, const char* gate, const char* ending)
    {
        namespace net = boost::asio;
        using Clock = std::chrono::steady_clock;
        std::ifstream input(payload, std::ios::binary);
        expect(input.good(), "fixture payload missing");
        std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        net::io_context io;
        net::ip::tcp::acceptor acceptor(io);
        acceptor.open(net::ip::tcp::v4());
        acceptor.set_option(net::socket_base::reuse_address(true)); // Only after prior server process exits.
        acceptor.bind({net::ip::address_v4::loopback(), static_cast<unsigned short>(std::stoi(port))});
        acceptor.listen(8);
        acceptor.non_blocking(true);
        std::cout << "READY port=" << acceptor.local_endpoint().port() << std::endl;
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        net::ip::tcp::socket peer(io);
        boost::system::error_code error;
        for (;;)
        {
            acceptor.accept(peer, error);
            if (!error) break;
            if (error != net::error::would_block && error != net::error::try_again)
                throw boost::system::system_error(error, "fixture.accept");
            expect(Clock::now() < deadline, "fixture accept timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (!std::filesystem::exists(gate))
        {
            expect(Clock::now() < deadline, "fixture gate timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        net::write(peer, net::buffer(bytes));
        if (std::string_view(ending) == "rst")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            peer.set_option(net::socket_base::linger(true, 0));
            peer.close();
        }
        else
        {
            peer.shutdown(net::ip::tcp::socket::shutdown_send);
            peer.non_blocking(true);
            char byte;
            for (;;)
            {
                peer.read_some(net::buffer(&byte, 1), error);
                if (error == net::error::eof) break;
                if (error && error != net::error::would_block && error != net::error::try_again)
                    throw boost::system::system_error(error, "fixture.fin");
                expect(Clock::now() < deadline, "fixture FIN timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::cout << "DONE " << ending << std::endl;
        return 0;
    }
}

int main(int argc, char** argv)
{
    using Client = elevsafe::RetinaClient;
    using Budget = elevsafe::RadarReconnectBudget;
    using Clock = Budget::Clock;
    try
    {
        if (argc == 6 && std::string_view(argv[1]) == "--server")
            return serve(argv[2], argv[3], argv[4], argv[5]);
        if (argc == 2 && std::string_view(argv[1]) == "--publisher")
        {
            testPeriodicPublisherFailure();
            return 0;
        }
        expect(argc == 1, "expected no arguments, --publisher, or --server PORT PAYLOAD GATE fin|rst");
        const Clock::time_point origin{};
        Budget count;
        expect(count.schedule(origin, false) == origin + std::chrono::seconds(1), "retry delay");
        expect(count.schedule(origin + std::chrono::seconds(2), true).has_value(), "second outage");
        expect(count.schedule(origin + std::chrono::seconds(4), true).has_value(), "third retry");
        expect(!count.schedule(origin + std::chrono::seconds(6), true), "healthy connection reset global retry cap");
        Budget budget;
        budget.schedule(origin, false);
        expect(!budget.schedule(origin + std::chrono::seconds(14), false), "recovery time budget not enforced");

        Client::RunDiagnostics status;
        status.stage = "connect";
#ifdef _WIN32
        status.errorCode = WSAEFAULT;
        expect(!Client::retryable(status, true), "10014 must not be retried");
        status.errorCode = WSAEADDRINUSE;
        expect(!Client::retryable(status, true), "10048 must not be retried");
        status.errorCode = WSAECONNREFUSED;
#else
        status.errorCode = ECONNREFUSED;
#endif
        expect(Client::retryable(status, false), "refusal must be recoverable");
        status.stage = "resolve";
        expect(!Client::retryable(status, true), "resolution is not retryable");
        status.stage = "eof";
        expect(!Client::retryable(status, false) && Client::retryable(status, true), "EOF opt-in");
        status.cancelled = true;
        expect(!Client::retryable(status, true), "cancellation must not reconnect");

        std::ostringstream log;
        Client client("127.0.0.1", 1);
        Client::SessionControl control;
        control.cancelRequested = [] { return true; };
        expect(client.runSession(log, control, status) == 0 && status.cancelled && !status.connected,
            "pre-connect cancellation");
        control.cancelRequested = {};
        control.connectNotAfter = Clock::now() - std::chrono::seconds(1);
        expect(client.runSession(log, control, status) == 1 && status.timedOut && status.stage == "start",
            "expired recovery deadline");

        // Kernel loopback connect completes without a server worker; no application bytes follow.
        // This deliberately exercises cancellable receive with no idle timeout.
        namespace net = boost::asio;
        net::io_context io;
        net::ip::tcp::acceptor listener(io);
        listener.open(net::ip::tcp::v4());
        listener.bind({net::ip::address_v4::loopback(), 0});
        listener.listen(8);
        Client idle("127.0.0.1", listener.local_endpoint().port());
        control.connectNotAfter.reset();
        const auto start = Clock::now();
        control.cancelRequested = [start] { return Clock::now() - start >= std::chrono::milliseconds(200); };
        expect(idle.runSession(log, control, status) == 0 && status.cancelled && status.connected
            && status.stage == "stopped", "idle receive cancellation");
        expect(Clock::now() - start < std::chrono::seconds(2), "receive cancellation took too long");
        testReceiveIdle();
        std::cout << "retina_reconnect_test passed: policy, 10014 rejection, expired deadline, owner-thread receive cancellation\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "retina_reconnect_test failed: " << error.what() << '\n';
        return 1;
    }
}
