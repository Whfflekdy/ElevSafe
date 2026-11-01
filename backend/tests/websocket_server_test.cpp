#include "elevsafe/websocket_server.h"
#include "elevsafe/websocket_message.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <syncstream>
#include <thread>
#include <utility>

namespace
{
    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace ws = beast::websocket;
    using Tcp = net::ip::tcp;
    using Error = boost::system::error_code;

    bool diagnostics = false;

    long long timeMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    class CapturedLog : public std::ostream
    {
        class Buffer : public std::streambuf
        {
            std::mutex mutex;
            std::string text;
            std::streamsize xsputn(const char* data, std::streamsize size) override
            {
                std::lock_guard lock(mutex);
                text.append(data, static_cast<std::size_t>(size));
                if (diagnostics) std::osyncstream(std::cerr).write(data, size);
                return size;
            }
            int_type overflow(int_type character) override
            {
                if (!traits_type::eq_int_type(character, traits_type::eof()))
                {
                    const char value = traits_type::to_char_type(character);
                    xsputn(&value, 1);
                }
                return traits_type::not_eof(character);
            }
        public:
            std::string str()
            {
                std::lock_guard lock(mutex);
                return text;
            }
        } buffer;
    public:
        CapturedLog() : std::ostream(nullptr) { rdbuf(&buffer); }
        std::string str() { return buffer.str(); }
    };

    std::string describe(const elevsafe::WebSocketStatistics& s)
    {
        std::ostringstream text;
        text << "connected=" << s.connected << ",accepted=" << s.accepted
             << ",disconnected=" << s.disconnected << ",sent=" << s.snapshotsSent
             << ",ingressReplaced=" << s.ingressReplaced << ",sessionReplaced=" << s.sessionReplaced
             << ",discarded=" << s.discardedSnapshots << ",writeTimeouts=" << s.writeTimeouts
             << ",queueFull=" << s.commandQueueFull << ",commands=" << s.commandsReceived
             << ",rejected=" << s.rejectedRequests << ",ingressPending=" << s.ingressPending
             << ",sessionPending=" << s.sessionPending << ",activeWrites=" << s.activeWrites
             << ",peakIngress=" << s.peakIngressPending << ",peakPending=" << s.peakSessionPending
             << ",peakActive=" << s.peakActiveWrites;
        return text.str();
    }

    struct TestContext
    {
        struct Snapshot
        {
            std::string stage = "setup", expected = "n/a", observed = "n/a", statistics = "n/a";
            std::uint16_t port = 0;
            long long time = timeMs();
        } current;
        const char* name;
        CapturedLog log;
        const elevsafe::WebSocketServer* server = nullptr;
        std::optional<Snapshot> firstTimeout;

        explicit TestContext(const char* test) : name(test) {}
        void refresh()
        {
            if (server) current.statistics = describe(server->statistics());
        }
        void print(const char* prefix, const Snapshot& s, const std::string& extra = {})
        {
            std::osyncstream(std::cerr) << prefix << " subtest=" << name << " stage=" << s.stage
                << " port=" << s.port << " timeMs=" << s.time << " thread=" << std::this_thread::get_id()
                << " expected=[" << s.expected << "] observed=[" << s.observed
                << "] stats=[" << s.statistics << "] " << extra << '\n';
        }
        void at(const char* operation, std::uint16_t endpoint, const std::string& expected = "operation completes")
        {
            current.stage = operation;
            current.port = endpoint;
            current.expected = expected;
            current.observed = "pending";
            current.time = timeMs();
            refresh();
            if (diagnostics) print("PROGRESS", current);
        }
        void timeout(const std::string& reason)
        {
            current.time = timeMs();
            refresh();
            if (!firstTimeout) firstTimeout = current;
            print("TIMEOUT", current, reason);
            std::osyncstream(std::cerr) << "--- server log before cleanup ---\n" << log.str()
                << "--- end server log before cleanup ---\n";
        }
        void cleanup(const std::string& detail)
        {
            if (diagnostics || firstTimeout)
                std::osyncstream(std::cerr) << "CLEANUP subtest=" << name << " timeMs=" << timeMs()
                    << " thread=" << std::this_thread::get_id() << " detail=" << detail << '\n';
        }
    };

    void expect(bool value, const char* text) { if (!value) throw std::runtime_error(text); }

    template<class Predicate>
    void until(TestContext& context, const elevsafe::WebSocketServer& server,
        const char* stage, const char* expected, Predicate predicate)
    {
        context.at(stage, server.port(), expected);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;)
        {
            const auto statistics = server.statistics();
            context.current.observed = describe(statistics);
            context.current.statistics = context.current.observed;
            if (predicate(statistics)) return;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                context.timeout("state transition deadline exceeded");
                throw std::runtime_error("state transition deadline exceeded");
            }
            std::this_thread::yield();
        }
    }

    class Client
    {
        TestContext& context;
        std::uint16_t port;
        using Response = beast::http::response<beast::http::string_body>;
    public:
        net::io_context io;
        ws::stream<beast::tcp_stream> stream{io};
        beast::flat_buffer input;

        explicit Client(TestContext& test, std::uint16_t endpoint, const char* path = "/radar",
            std::shared_ptr<Response> response = nullptr) : context(test), port(endpoint)
        {
            perform("client.connect", [&](auto handler)
            {
                stream.next_layer().async_connect(
                    Tcp::endpoint(net::ip::make_address("127.0.0.1"), port), handler);
            });
            if (!path) return; // Raw TCP peer for the pending HTTP handshake shutdown test.
            if (!response) response = std::make_shared<Response>();
            perform("client.handshake", [&, response](auto handler)
            {
                stream.async_handshake(*response, "127.0.0.1", path,
                    [handler, response](Error error) { handler(error); });
            });
        }
        ~Client()
        {
            context.cleanup("client.socket.close.begin port=" + std::to_string(port));
            Error ignored;
            stream.next_layer().socket().close(ignored);
            context.cleanup("client.socket.close.done code=" + std::to_string(ignored.value())
                + "; member io_context destruction follows");
        }
        template<class Start>
        void perform(const char* stage, Start start)
        {
            context.at(stage, port);
            struct Operation { bool done = false; Error outcome; };
            auto operation = std::make_shared<Operation>();
            io.restart();
            start([operation](Error error) { operation->outcome = error; operation->done = true; });
            io.run_for(std::chrono::seconds(3));
            if (!operation->done)
            {
                context.current.observed = "completion=false";
                context.timeout("client operation deadline exceeded");
                Error ignored;
                stream.next_layer().socket().cancel(ignored);
                context.cleanup("client.cancel code=" + std::to_string(ignored.value()));
                stream.next_layer().socket().close(ignored);
                context.at("client.cancel.drain", port, "canceled operation callback completes");
                auto nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                // Keep this client and every buffer alive until completion. A stuck
                // native cancellation is diagnosed by the external process watchdog.
                while (!operation->done)
                {
                    io.restart();
                    io.run_for(std::chrono::milliseconds(250));
                    if (!operation->done && std::chrono::steady_clock::now() >= nextReport)
                    {
                        context.current.observed = "completion=false; native cancellation pending";
                        context.current.time = timeMs();
                        context.refresh();
                        context.print("PENDING", context.current);
                        nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    }
                    std::this_thread::yield();
                }
                context.cleanup("client.cancel.callback.done code=" + std::to_string(operation->outcome.value()));
                throw std::runtime_error("client operation deadline exceeded");
            }
            if (operation->outcome) throw boost::system::system_error(operation->outcome, stage);
        }
        void send(const std::string& message)
        {
            auto owned = std::make_shared<const std::string>(message);
            perform("client.write", [&, owned](auto handler)
            {
                stream.async_write(net::buffer(*owned), [handler, owned](Error error, std::size_t) { handler(error); });
            });
        }
        std::string read(const char* stage = "client.read")
        {
            input.consume(input.size());
            perform(stage, [&](auto handler)
            {
                stream.async_read(input, [handler](Error error, std::size_t) { handler(error); });
            });
            return beast::buffers_to_string(input.data());
        }
        void close()
        {
            perform("client.close", [&](auto handler) { stream.async_close(ws::close_code::normal, handler); });
        }
    };

    elevsafe::WebSocketConfig testConfig(std::uint16_t port = 8080)
    {
        elevsafe::WebSocketConfig config;
        config.port = port;
        return config;
    }

    void testHandshakeSnapshotsAndCommands(TestContext& context)
    {
        elevsafe::WebSocketServer server(testConfig(), context.log);
        context.server = &server;
        context.at("server.start", 8080);
        server.start();
        auto collision = testConfig();
        collision.port = server.port();
        bool failed = false;
        elevsafe::WebSocketServer duplicate(collision, context.log);
        context.at("collision.start", server.port());
        try { duplicate.start(); }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != net::error::address_in_use) throw;
            failed = true;
        }
        duplicate.stop();
        duplicate.stop();
        expect(failed, "port collision accepted");
        bool rejectedPath = false;
        auto response = std::make_shared<beast::http::response<beast::http::string_body>>();
        try { Client bad(context, server.port(), "/wrong", response); }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != ws::error::upgrade_declined) throw;
            rejectedPath = true;
        }
        expect(rejectedPath, "wrong path accepted");
        expect(response->result() == beast::http::status::not_found, "wrong path must return HTTP 404");

        Client first(context, server.port());
        Client second(context, server.port());
        until(context, server, "broadcast.clients.ready", "connected=2",
            [](const auto& s) { return s.connected == 2; });
        const auto snapshot = elevsafe::mockSnapshot();
        expect(server.publishSnapshot(snapshot), "publish failed");
        expect(first.read() == snapshot && second.read() == snapshot, "broadcast content changed");
        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
            first.send(nlohmann::json({{"door_command", command}}).dump());
        until(context, server, "commands.received", "commandsReceived=3",
            [](const auto& s) { return s.commandsReceived == 3; });
        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
        {
            const auto request = server.popCommand();
            expect(request.has_value(), "command missing");
            expect(request->command == command, "command FIFO changed");
        }
        expect(!server.popCommand().has_value(), "unexpected command");
        for (const auto* invalid : {"{", "[]", "{}", "{\"door_command\":\"INVALID\"}"}) first.send(invalid);
        until(context, server, "commands.invalid.rejected", "rejectedRequests=4",
            [](const auto& s) { return s.rejectedRequests == 4; });
        first.close();
        until(context, server, "broadcast.one-client.remains", "connected=1",
            [](const auto& s) { return s.connected == 1; });
        const auto nextSnapshot = elevsafe::mockSnapshot(100000);
        expect(server.publishSnapshot(nextSnapshot), "surviving client publish failed");
        expect(second.read() == nextSnapshot, "one disconnect interrupted another client");
        second.close();
        until(context, server, "broadcast.clients.closed", "connected=0",
            [](const auto& s) { return s.connected == 0; });
        Client reconnected(context, server.port());
        until(context, server, "reconnected.client.ready", "connected=1",
            [](const auto& s) { return s.connected == 1; });
        expect(server.publishSnapshot(nextSnapshot), "reconnected publish failed");
        expect(reconnected.read() == nextSnapshot, "reconnected client failed");
        context.at("server.stop", server.port());
        server.stop(); // Peer is idle with no close reply: bounded shutdown still completes.
        server.stop();
        expect(server.statistics().connected == 0, "sessions remain after shutdown");
        expect(!server.publishSnapshot(snapshot), "stopped server accepted publish");
        context.refresh();
    }

    void testQueueBoundsAndSlowClient(TestContext& context)
    {
        auto config = testConfig(8081);
        config.maxCommands = 2;
        config.maxClients = 2;
        elevsafe::WebSocketServer server(config, context.log);
        context.server = &server;
        context.at("server.start", config.port);
        server.start();
        Client sender(context, server.port());
        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
            sender.send(nlohmann::json({{"door_command", command}}).dump());
        until(context, server, "commands.overflow.closed", "queueFull=1, connected=0, disconnected=1",
            [](const auto& s) { return s.commandQueueFull == 1 && s.connected == 0 && s.disconnected == 1; });
        auto first = server.popCommand();
        auto second = server.popCommand();
        expect(first.has_value() && second.has_value(), "bounded command queue lost stored requests");
        expect(first->command == "OPEN" && second->command == "HOLD", "commands silently overwritten");
        expect(!server.popCommand().has_value(), "command queue exceeded bound");

        Client slow(context, server.port());
        until(context, server, "slow.client.ready", "accepted=2, connected=1",
            [](const auto& s) { return s.accepted == 2 && s.connected == 1; });
        auto large = nlohmann::json::parse(elevsafe::mockSnapshot());
        large["passengers"][0]["label"] = std::string(60000, 'x');
        const auto text = large.dump();
        context.at("slow.snapshot.burst", server.port(), "bounded queues while publishing 1000 snapshots");
        for (int i = 0; i < 1000; ++i)
        {
            expect(server.publishSnapshot(text), "bounded publish failed");
            const auto s = server.statistics();
            context.current.observed = describe(s);
            expect(s.peakIngressPending <= 1 && s.peakSessionPending <= config.maxClients
                && s.peakActiveWrites <= config.maxClients, "snapshot storage exceeded configured bounds");
        }
        until(context, server, "snapshot.pressure.observed", "replacement or close-discard count > 0",
            [](const auto& s) { return s.ingressReplaced + s.sessionReplaced + s.discardedSnapshots > 0; });
        Client fast(context, server.port());
        until(context, server, "fast.client.ready", "accepted=3; slow may remain or be removed",
            [](const auto& s) { return s.accepted == 3; });
        const auto small = elevsafe::mockSnapshot();
        expect(server.publishSnapshot(small), "fast client publish failed");
        auto delivered = fast.read("fast.snapshot.receive");
        if (delivered != small) delivered = fast.read("fast.snapshot.receive.latest");
        context.at("fast.snapshot.received", server.port(), "payload equals the published small snapshot");
        context.current.observed = delivered == small ? "payload matches" : "payload differs";
        expect(delivered == small, "slow client blocked another client");

        const auto beforeCommand = server.statistics().commandsReceived;
        fast.send("{\"door_command\":\"OPEN\"}");
        until(context, server, "fast.command.received", "one additional manual request received",
            [beforeCommand](const auto& s) { return s.commandsReceived == beforeCommand + 1; });
        auto fastRequest = server.popCommand();
        expect(fastRequest.has_value(), "fast client request missing");
        expect(fastRequest->command == "OPEN", "fast client request changed");
        until(context, server, "slow.client.retained-or-removed",
            "fast delivery verified; slow retained (connected=2,disconnected=1) or removed (1,2)",
            [](const auto& s)
            {
                return s.accepted == 3 && ((s.connected == 2 && s.disconnected == 1)
                    || (s.connected == 1 && s.disconnected == 2));
            });
        context.cleanup("slow status verified: " + describe(server.statistics()));
        expect(server.publishSnapshot(small), "post-status publish failed");
        const auto afterStatus = fast.read("fast.snapshot.after-slow-status");
        context.at("fast.snapshot.after-slow-status.verified", server.port(), "fast client still receives after status check");
        context.current.observed = afterStatus == small ? "payload matches" : "payload differs";
        expect(afterStatus == small, "fast client was lost while checking slow client status");
        expect(!server.publishSnapshot(std::string(65537, 'x')), "oversized snapshot accepted");
        context.at("server.stop", server.port());
        server.stop(); // Includes in-flight writes to a peer that never reads.
        expect(server.statistics().connected == 0, "pending write leaked session");
        const auto stopped = server.statistics();
        expect(stopped.ingressPending == 0 && stopped.sessionPending == 0 && stopped.activeWrites == 0,
            "snapshot storage remains after stop");
        server.stop();
        context.refresh();
    }

    void testOversizedRequestAndPendingHandshake(TestContext& context)
    {
        elevsafe::WebSocketServer server(testConfig(8082), context.log);
        context.server = &server;
        context.at("server.start", 8082);
        server.start();
        Client client(context, server.port());
        client.send(std::string(4097, 'x'));
        bool closed = false;
        try { client.read(); }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != ws::error::closed && error.code() != net::error::eof
                && error.code() != net::error::connection_reset) throw;
            closed = true;
        }
        expect(closed, "oversized request must close the connection");
        until(context, server, "oversized.request.rejected", "rejectedRequests=1",
            [](const auto& s) { return s.rejectedRequests == 1; });
        until(context, server, "oversized.client.closed", "connected=0",
            [](const auto& s) { return s.connected == 0; });
        Client pending(context, server.port(), nullptr);
        context.at("server.stop", server.port());
        server.stop();
        expect(server.statistics().connected == 0, "pending handshake leaked");
        context.refresh();
    }
    void testFailedStartCleanup(TestContext& context)
    {
#ifdef _WIN32
        // dump() rejects invalid UTF-8. Do not depend on Windows' UI language or
        // exact localized wording; exercise the same category used by sockets.
        for (const int value : {10014, 10054})
        {
            const Error error(value, boost::system::system_category());
            const auto text = error.message();
            expect(!text.empty(), "Windows socket error message must not be empty");
            nlohmann::json diagnostic = {{"code", value}, {"message", text}};
            expect(!diagnostic.dump().empty(), "Windows socket error must be UTF-8");
        }
#endif
        auto config = testConfig();
        config.address = "invalid-ip";
        elevsafe::WebSocketServer server(config, context.log);
        context.server = &server;
        context.at("invalid-address.start", config.port);
        bool failed = false;
        try { server.start(); }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != net::error::invalid_argument) throw;
            failed = true;
        }
        expect(failed, "invalid startup succeeded");
        context.at("failed-start.stop", config.port);
        server.stop();
        server.stop();
        expect(server.statistics().connected == 0, "failed startup retained sessions");
        expect(!server.publishSnapshot(elevsafe::mockSnapshot()), "failed startup accepted publish");
        context.refresh();
    }

    // Explicit opt-in: connects to a separately launched backend --ws-mock.
    // The normal CTest cases continue to own their own servers.
    void testMockProcess(TestContext& context)
    {
        Client first(context, 8080);
        Client second(context, 8080);
        auto read = [](Client& client)
        {
            auto text = client.read("mock.snapshot.receive");
            expect(elevsafe::isSnapshot(text), "invalid v6 mock snapshot");
            return text;
        };
        auto elapsed = [](const std::string& text)
        {
            return nlohmann::json::parse(text)["timestamp_us"].get<std::uint64_t>();
        };
        auto a = read(first);
        auto b = read(second);
        // First client can receive frames while the second is handshaking.
        // Drain only this bounded initial offset; never retry failed connections.
        for (int i = 0; a != b && i < 10; ++i)
        {
            if (elapsed(a) < elapsed(b)) a = read(first);
            else if (elapsed(b) < elapsed(a)) b = read(second);
            else break;
        }
        expect(a == b, "clients failed to align on identical mock snapshot");
        const auto startUs = elapsed(a);
        auto previousUs = startUs;
        const auto start = std::chrono::steady_clock::now();
        constexpr int intervals = 20;
        for (int i = 0; i < intervals; ++i)
        {
            a = read(first);
            b = read(second);
            expect(a == b, "mock broadcast differs between clients");
            expect(elapsed(a) > previousUs, "mock relative time did not advance");
            previousUs = elapsed(a);
        }
        const auto wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto sourceMs = static_cast<double>(previousUs - startUs) / 1000;
        std::cout << "mock cadence intervals=" << intervals << " clients=2 matched=21"
                  << " meanReceiveMs=" << wallMs / intervals
                  << " meanSourceMs=" << sourceMs / intervals << '\n';
        // Scheduler tolerance for a local integration test, not a real-time SLA.
        // Both clocks must distinguish 10Hz from the previous 1Hz behavior.
        expect(wallMs / intervals >= 70 && wallMs / intervals <= 150, "mock receive cadence outside 70-150ms");
        expect(sourceMs / intervals >= 70 && sourceMs / intervals <= 150, "mock source cadence outside 70-150ms");
        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
            first.send(nlohmann::json({{"door_command", command}}).dump());
        first.send("{\"door_command\":\"INVALID\"}");
        first.close();
        read(second); // Remaining peer still receives after the other closes.
        second.close();
        Client reconnected(context, 8080);
        read(reconnected);
        reconnected.close();
    }

    bool runTest(const char* name, void (*test)(TestContext&))
    {
        TestContext context{name};
        if (diagnostics) context.print("TEST_BEGIN", context.current);
        try
        {
            test(context);
            if (diagnostics) context.print("TEST_END", context.current, "result=PASS");
            return true;
        }
        catch (const boost::system::system_error& error)
        {
            context.print("ERROR", context.firstTimeout.value_or(context.current),
                "code=" + std::to_string(error.code().value()) + " category=" + error.code().category().name()
                + " message=" + error.what());
        }
        catch (const std::exception& error)
        {
            context.print("ERROR", context.firstTimeout.value_or(context.current),
                std::string("code=none message=") + error.what());
        }
        std::cerr << "--- server log ---\n" << context.log.str() << "--- end server log ---\n";
        return false;
    }
}

int main(int argc, char** argv)
{
    std::string selected;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument(argv[i]);
        if (argument == "--diagnostics") diagnostics = true;
        else if (argument == "--case" && i + 1 < argc) selected = argv[++i];
        else { std::cerr << "Usage: websocket_server_test [--diagnostics] [--case NAME]\n"; return 2; }
    }
    struct Case { const char* name; void (*test)(TestContext&); };
    if (selected == "mock-process") return runTest("mock-process", testMockProcess) ? 0 : 1;
    const Case cases[] = {
        {"failed-start-cleanup", testFailedStartCleanup},
        {"handshake-snapshots-commands", testHandshakeSnapshotsAndCommands},
        {"queue-bounds-slow-client", testQueueBoundsAndSlowClient},
        {"oversized-pending-handshake", testOversizedRequestAndPendingHandshake}
    };
    bool found = false;
    for (const auto& entry : cases)
    {
        if (!selected.empty() && selected != entry.name) continue;
        found = true;
        if (!runTest(entry.name, entry.test)) return 1;
    }
    if (!found) { std::cerr << "Unknown test case: " << selected << '\n'; return 2; }
    std::cout << "websocket_server_test passed\n";
}
