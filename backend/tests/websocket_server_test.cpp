#include "elevsafe/door_simulation.h"
#include "elevsafe/mock_door_policy.h"
#include "elevsafe/websocket_server.h"
#include "elevsafe/websocket_message.h"
#include "websocket_mock_scenarios.h"

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

    elevsafe::WebSocketConfig testConfig()
    {
        elevsafe::WebSocketConfig config;
        config.port = 0; // Each owned fixture asks the OS for an available port.
        return config;
    }

    void startServer(TestContext& context, elevsafe::WebSocketServer& server,
        const elevsafe::WebSocketConfig& config, const char* stage = "server.start")
    {
        context.at(stage, config.port);
        context.log << stage << " requestedPort=" << config.port << " boundPort=unassigned\n";
        server.start();
        const auto boundPort = server.port();
        context.log << stage << " requestedPort=" << config.port << " boundPort=" << boundPort << '\n';
        context.at("server.started", boundPort, "assigned port is nonzero");
        expect(boundPort != 0, "server did not receive an assigned port");
    }

    void testHandshakeSnapshotsAndCommands(TestContext& context)
    {
        const auto config = testConfig();
        elevsafe::WebSocketServer server(config, context.log);
        context.server = &server;
        startServer(context, server, config);
        auto collision = testConfig();
        collision.port = server.port();
        bool failed = false;
        elevsafe::WebSocketServer duplicate(collision, context.log);
        try { startServer(context, duplicate, collision, "collision.start"); }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != net::error::address_in_use) throw;
            context.log << "collision rejected requestedPort=" << collision.port
                << " code=" << error.code().value() << " category=" << error.code().category().name() << '\n';
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
        auto unknown = nlohmann::json::parse(snapshot);
        unknown["door_state"] = "UNKNOWN";
        const auto unknownText = unknown.dump();
        expect(server.publishSnapshot(unknownText), "UNKNOWN publish rejected");
        expect(first.read() == unknownText && second.read() == unknownText,
            "UNKNOWN state was converted or not broadcast");
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

    void testMockScenarios(TestContext& context)
    {
        const auto config = testConfig();
        elevsafe::WebSocketServer server(config, context.log);
        context.server = &server;
        startServer(context, server, config);
        Client first(context, server.port());
        Client second(context, server.port());
        until(context, server, "scenarios.clients.ready", "connected=2",
            [](const auto& s) { return s.connected == 2; });

        const auto scenarios = elevsafe::test::mockScenarios();
        std::uint64_t previousUs = 0;
        for (std::size_t i = 0; i < scenarios.size(); ++i)
        {
            const auto& scenario = scenarios[i];
            const auto text = scenario.snapshot.dump();
            context.at("scenario.publish", server.port(), scenario.name);
            expect(server.publishSnapshot(text), "valid scenario rejected by publisher");
            // Read both before publishing the next scenario: the intentional latest-only
            // queue policy must not make this content test depend on scheduling races.
            const auto a = first.read("scenario.first.receive");
            const auto b = second.read("scenario.second.receive");
            expect(a == text && b == text, "scenario content/order differs across clients");
            expect(elevsafe::isSnapshot(a) && elevsafe::isSnapshot(b), "received invalid v6 scenario");
            const auto elapsedUs = nlohmann::json::parse(a)["timestamp_us"].get<std::uint64_t>();
            expect(i == 0 ? elapsedUs == 0 : elapsedUs > previousUs, "received scenario time did not advance");
            previousUs = elapsedUs;
            std::cout << "mock scenario received: " << scenario.name << " clients=2 passengers="
                      << scenario.snapshot["passengers"].size() << " timestamp_us=" << elapsedUs << '\n';
        }

        context.at("scenario.invalid.publish", server.port());
        expect(!server.publishSnapshot("{"), "malformed snapshot accepted by publisher");
        for (const auto& value : nlohmann::json::array({-1, 0.5, 1.0, "100000", nullptr, true}))
        {
            auto invalid = scenarios.back().snapshot;
            invalid["timestamp_us"] = value;
            expect(!server.publishSnapshot(invalid.dump()), "invalid elapsed time accepted by publisher");
        }
        auto invalid = scenarios.back().snapshot;
        invalid["timestamp"] = 123;
        expect(!server.publishSnapshot(invalid.dump()), "non-string timestamp accepted by publisher");
        invalid.erase("timestamp_us");
        invalid["timestamp"] = "2026-10-30T11:00:00Z";
        expect(!server.publishSnapshot(invalid.dump()), "missing elapsed time accepted by publisher");
        const auto finalSnapshot = scenarios.back().snapshot.dump();
        expect(server.publishSnapshot(finalSnapshot), "invalid publish damaged server");
        expect(first.read() == finalSnapshot && second.read() == finalSnapshot,
            "invalid snapshot was sent or interrupted valid delivery");

        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
            first.send(nlohmann::json({{"door_command", command}}).dump());
        until(context, server, "scenarios.commands.received", "commandsReceived=3",
            [](const auto& s) { return s.commandsReceived == 3; });
        first.close();
        until(context, server, "scenarios.sender.closed", "connected=1",
            [](const auto& s) { return s.connected == 1; });
        std::optional<std::uint64_t> sender;
        for (const auto* command : {"OPEN", "HOLD", "CLOSE"})
        {
            const auto request = server.popCommand();
            expect(request.has_value() && request->command == command,
                "accepted command lost/reordered after sender disconnect");
            if (!sender) sender = request->clientId;
            expect(request->clientId == *sender, "command sender changed");
        }
        expect(!server.popCommand(), "unexpected command after FIFO drain");
        second.close();
        until(context, server, "scenarios.clients.closed", "connected=0",
            [](const auto& s) { return s.connected == 0; });
        context.at("server.stop", server.port());
        server.stop();
        const auto stopped = server.statistics();
        expect(stopped.connected == 0 && stopped.ingressPending == 0
            && stopped.sessionPending == 0 && stopped.activeWrites == 0, "scenario shutdown retained work");
        context.refresh();
        std::cout << "mock scenarios passed=6 invalidPublishRejected=9 FIFO-after-disconnect=OPEN,HOLD,CLOSE\n";
    }

    void testDoorRiskScenarios(TestContext& context)
    {
        using D = elevsafe::DoorSimulation;
        using namespace std::chrono_literals;
        auto config = testConfig(); config.allowNullDoorRequest = true;
        elevsafe::WebSocketServer server(config, context.log); context.server = &server;
        startServer(context, server, config);
        Client first(context, server.port()), second(context, server.port());
        until(context, server, "risk.clients.ready", "connected=2", [](const auto& s) { return s.connected == 2; });
        std::uint64_t sequence = 0;
        for (const auto name : elevsafe::mockDoorScenarioNames)
        {
            D door(name == "unknown" ? D::State::Unknown : D::State::Closed);
            elevsafe::MockDoorPolicy policy;
            const D::Time zero{};
            const auto publish = [&]()
            {
                auto frame = nlohmann::json::parse(elevsafe::mockSnapshot(sequence++ * 100000));
                frame["passengers"] = policy.passengers(); frame["door_state"] = D::name(door.state());
                if (const auto command = door.takeAppliedCommand()) frame["door_command"] = D::name(*command);
                const auto text = frame.dump();
                expect(server.publishSnapshot(text), "risk snapshot rejected");
                expect(first.read() == text && second.read() == text, "risk snapshot broadcast mismatch");
                return frame;
            };
            policy.accept(elevsafe::mockDoorScenarioInput(name, 0us)); policy.tick(door, zero);
            auto initial = publish();
            if (name == "unknown")
            {
                expect(initial["door_state"] == "UNKNOWN" && initial["door_command"].is_null(), "UNKNOWN wire mismatch");
                policy.tick(door, zero + 1s);
                expect(publish()["door_state"] == "UNKNOWN", "UNKNOWN advanced without manual");
            }
            if (name == "normal" || name == "unknown" || name == "inside-fall" || name == "inside-immobile")
            {
                const auto before = server.statistics().commandsReceived;
                first.send("{\"door_command\":\"OPEN\"}");
                until(context, server, "risk.manual.received", "new OPEN", [&](const auto& s) { return s.commandsReceived == before + 1; });
                const auto request = server.popCommand(); expect(request && request->command == "OPEN", "manual FIFO missing");
                door.apply(D::Command::Open, zero + 1s); policy.tick(door, zero + 1s);
                expect(publish()["door_state"] == "OPENING", "manual opening missing");
            }
            policy.tick(door, zero + 3680ms);
            expect(publish()["door_state"] == "OPEN", "risk/normal did not open");
            const auto beforeReject = server.statistics().rejectedRequests;
            first.send("{\"door_command\":null}");
            // A later valid frame is a processing barrier for the preceding null.
            const auto before = server.statistics().commandsReceived;
            first.send("{\"door_command\":\"HOLD\"}");
            until(context, server, "risk.null.barrier", "new HOLD", [&](const auto& s) { return s.commandsReceived == before + 1; });
            const auto hold = server.popCommand(); expect(hold && hold->command == "HOLD" && !server.popCommand(), "null entered FIFO");
            expect(server.statistics().rejectedRequests == beforeReject, "null rejected");
            // Only the manual-conflict script consumes this barrier as an action.
            if (name == "manual-conflict")
            {
                door.apply(D::Command::Hold, zero + 4s); policy.tick(door, zero + 4s); publish();
                second.close();
                policy.tick(door, zero + 5s);
                expect(door.mode() == D::Mode::Manual && door.activeCommand() == D::Command::Hold, "disconnect released HOLD");
                Client replacement(context, server.port()); replacement.close();
                // Keep the other broadcast tests intact; this scenario is last.
                policy.accept(nlohmann::json::array()); policy.tick(door, zero + 1h);
                expect(door.mode() == D::Mode::Manual, "clear released manual HOLD");
                std::cout << "door policy scenario received: manual-conflict clients=2; disconnect preserved HOLD\n";
                break;
            }
            policy.accept(elevsafe::mockDoorScenarioInput(name, 8s)); policy.tick(door, zero + 8s);
            publish();
            policy.tick(door, zero + 12s); const auto closing = publish();
            policy.tick(door, zero + 14350ms); const auto final = publish();
            if (name == "risk-clear")
            {
                expect(closing["door_state"] == "CLOSING" && closing["door_command"] == "CLOSE", "clear wire close missing");
                expect(final["door_state"] == "CLOSED", "clear wire completion missing");
            }
            if (name == "input-loss") expect(final["door_state"] == "OPEN" && policy.inputMissing(), "loss released hold");
            std::cout << "door policy scenario received: " << name << " clients=2\n";
        }
        first.close(); server.stop(); expect(server.statistics().connected == 0, "risk sessions remain");
    }

    void testQueueBoundsAndSlowClient(TestContext& context)
    {
        auto config = testConfig();
        config.maxCommands = 2;
        config.maxClients = 2;
        elevsafe::WebSocketServer server(config, context.log);
        context.server = &server;
        startServer(context, server, config);
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
        const auto config = testConfig();
        elevsafe::WebSocketServer server(config, context.log);
        context.server = &server;
        startServer(context, server, config);
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
        bool failed = false;
        try { startServer(context, server, config, "invalid-address.start"); }
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

    void testNullCompatibility(TestContext& context)
    {
        // The default configuration is shared by basic Mock and Radar modes.
        for (const bool allow : {false, true})
        {
            auto config = testConfig();
            config.allowNullDoorRequest = allow;
            elevsafe::WebSocketServer server(config, context.log);
            context.server = &server;
            startServer(context, server, config);
            Client sender(context, server.port()), other(context, server.port());
            using D = elevsafe::DoorSimulation;
            D model;
            const D::Time zero{};
            std::size_t sent = 0, rejected = 0;
            auto command = [&](const char* text, D::Command value, D::Time now)
            {
                sender.send(nlohmann::json({{"door_command", text}}).dump());
                ++sent;
                until(context, server, "null-test.command", "command queued",
                    [&](const auto& stats) { return stats.commandsReceived == sent; });
                const auto request = server.popCommand();
                expect(request && request->command == text, "FIFO changed");
                model.apply(value, now);
            };
            auto nulls = [&]()
            {
                const auto beforeState = model.state();
                const auto beforeMode = model.mode();
                const auto beforeDeadline = model.deadline();
                const auto beforeActive = model.activeCommand();
                const auto beforeHold = model.openHoldDeadline();
                for (int i = 0; i < 3; ++i) other.send("{\"door_command\":null}");
                // Same-connection invalid sentinel proves prior nulls have been processed.
                other.send("{}");
                rejected += allow ? 1 : 4;
                until(context, server, "null-test.barrier", "requests processed",
                    [&](const auto& stats) { return stats.rejectedRequests == rejected; });
                expect(server.statistics().commandsReceived == sent, "null counted as command");
                expect(!server.popCommand(), "null entered FIFO");
                expect(model.state() == beforeState && model.mode() == beforeMode
                    && model.deadline() == beforeDeadline && model.activeCommand() == beforeActive
                    && model.openHoldDeadline() == beforeHold,
                    "null changed simulation");
            };
            nulls(); // CLOSED/AUTO
            command("OPEN", D::Command::Open, zero);
            nulls(); // Another client cannot cancel OPENING or restart its timer.
            model.update(zero + D::openDuration - std::chrono::microseconds(1));
            expect(model.state() == D::State::Opening, "early open");
            model.update(zero + D::openDuration);
            expect(model.state() == D::State::Open && model.mode() == D::Mode::Auto, "open deadline");
            nulls();
            command("CLOSE", D::Command::Close, zero + std::chrono::seconds(3));
            nulls();
            model.update(zero + std::chrono::seconds(3) + D::closeDuration);
            expect(model.state() == D::State::Closed && model.mode() == D::Mode::Auto, "close deadline");
            nulls();
            command("HOLD", D::Command::Hold, zero + std::chrono::seconds(6));
            nulls();
            model.update(zero + std::chrono::seconds(9));
            nulls();
            expect(model.mode() == D::Mode::Manual && model.activeCommand() == D::Command::Hold, "null released HOLD");
            command("CLOSE", D::Command::Close, zero + std::chrono::seconds(10));
            command("OPEN", D::Command::Open, zero + std::chrono::seconds(11));
            command("CLOSE", D::Command::Close, zero + std::chrono::seconds(12));
            nulls(); // Delayed notification for superseded OPEN.
            expect(model.deadline() == zero + std::chrono::seconds(12) + D::closeDuration, "delayed null cancelled close");
            model.update(zero + std::chrono::seconds(15));
            expect(model.state() == D::State::Closed && model.mode() == D::Mode::Auto, "final close");
            for (const auto* invalid : {"{\"door_command\":\"AUTO\"}", "{\"door_command\":false}",
                "{\"door_command\":0}", "{\"door_command\":{}}", "{\"door_command\":null,\"extra\":true}"})
                other.send(invalid);
            rejected += 5;
            until(context, server, "null-test.invalid", "strict type validation",
                [&](const auto& stats) { return stats.rejectedRequests == rejected; });
            expect(!server.popCommand() && server.statistics().commandsReceived == sent, "invalid entered FIFO");
            sender.close(); other.close(); server.stop();
            context.server = nullptr;
        }
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
        first.send("{\"door_command\":null}");
        first.close();
        read(second); // Remaining peer still receives after the other closes.
        second.close();
        Client reconnected(context, 8080);
        read(reconnected);
        reconnected.close();
    }

    // Opt-in client for the product --ws-mock --ws-door-sim process.
    void testDoorSimProcess(TestContext& context)
    {
        Client first(context, 8080), second(context, 8080);
        auto read = [](Client& client)
        {
            const auto text = client.read("door-sim.snapshot");
            expect(elevsafe::isSnapshot(text), "invalid simulation snapshot");
            return nlohmann::json::parse(text);
        };
        auto a = read(first), b = read(second);
        for (int i = 0; a != b && i < 10; ++i)
        {
            if (a["timestamp_us"] < b["timestamp_us"]) a = read(first);
            else b = read(second);
        }
        expect(a == b, "simulation clients not aligned");
        expect(a["door_state"] == "CLOSED", "simulation not initially closed");
        auto pair = [&]()
        {
            auto next = read(first);
            expect(next == read(second), "simulation broadcast mismatch");
            expect(next["timestamp_us"] > a["timestamp_us"], "simulation time not increasing");
            a = next;
            return next;
        };
        auto transition = [&](const char* command, const char* moving, const char* target, double duration)
        {
            first.send(nlohmann::json({{"door_command", command}}).dump());
            const auto start = std::chrono::steady_clock::now();
            std::uint64_t movingUs = 0;
            int count = 0;
            bool sawCommand = false, completed = false;
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(6))
            {
                const auto next = pair();
                ++count;
                if (next["door_command"] == command) sawCommand = true;
                if (next["door_state"] == moving && !movingUs)
                {
                    movingUs = next["timestamp_us"].get<std::uint64_t>();
                    for (int i = 0; i < 3; ++i) second.send("{\"door_command\":null}");
                    std::cout << "sent repeated null during " << moving << " from second client\n";
                }
                if (movingUs && next["door_state"] == target)
                {
                    const double elapsedMs = (next["timestamp_us"].get<std::uint64_t>() - movingUs) / 1000.0;
                    expect(elapsedMs >= duration - 200 && elapsedMs <= duration + 400,
                        "observed transition duration outside snapshot tolerance");
                    std::cout << "door-sim command=" << command << " observedTransitionMs=" << elapsedMs
                              << " snapshots=" << count << " clients=2\n";
                    completed = true;
                    break;
                }
            }
            expect(completed && sawCommand, "missing simulation transition/command");
            second.send("{\"door_command\":null}");
            expect(pair()["door_command"].is_null(), "command repeated as ACK");
        };
        transition("OPEN", "OPENING", "OPEN", 2680);
        const auto openUs = a["timestamp_us"].get<std::uint64_t>();
        std::uint64_t closingUs = 0;
        int autoCloseEvents = 0;
        bool autoClosed = false;
        const auto autoStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - autoStart < std::chrono::seconds(8))
        {
            const auto next = pair();
            if (next["door_command"] == "CLOSE") ++autoCloseEvents;
            else expect(next["door_command"].is_null(), "unexpected AUTO-cycle command");
            if (next["door_state"] == "CLOSING" && !closingUs)
            {
                closingUs = next["timestamp_us"].get<std::uint64_t>();
                const auto holdMs = (closingUs - openUs) / 1000.0;
                expect(holdMs >= 3700 && holdMs <= 4300, "OPEN hold outside 10Hz tolerance");
                std::cout << "door-sim automatic openHoldMs=" << holdMs << '\n';
            }
            if (next["door_state"] == "CLOSED")
            {
                expect(closingUs != 0, "automatic close skipped CLOSING");
                const auto closeMs = (next["timestamp_us"].get<std::uint64_t>() - closingUs) / 1000.0;
                expect(closeMs >= 2150 && closeMs <= 2750, "automatic close duration");
                std::cout << "door-sim automatic closeMs=" << closeMs << " clients=2\n";
                autoClosed = true;
                break;
            }
            expect(closingUs || next["door_state"] == "OPEN", "unexpected AUTO-cycle state");
        }
        expect(autoClosed && autoCloseEvents == 1, "automatic CLOSE must publish exactly once");
        for (int i = 0; i < 5; ++i)
        {
            const auto next = pair();
            expect(next["door_state"] == "CLOSED" && next["door_command"].is_null(),
                "AUTO event replayed or door reopened");
        }
        transition("OPEN", "OPENING", "OPEN", 2680);
        transition("CLOSE", "CLOSING", "CLOSED", 2350);
        transition("HOLD", "OPENING", "OPEN", 2680);
        const auto startUs = a["timestamp_us"].get<std::uint64_t>();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) expect(pair()["door_state"] == "OPEN", "hold lost open");
        const auto wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 50;
        const auto source = (a["timestamp_us"].get<std::uint64_t>() - startUs) / 50000.0;
        expect(wall >= 70 && wall <= 150 && source >= 70 && source <= 150, "simulation not 10Hz");
        std::cout << "door-sim meanReceiveMs=" << wall << " meanSourceMs=" << source << '\n';
        first.send("{\"door_command\":null}"); // Valid no-op in simulation; server logs confirm no rejection.
        expect(pair()["door_state"] == "OPEN", "null changed state");
        first.send("{\"door_command\":\"OPEN\"}"); // Release HOLD without animation.
        bool released = false;
        for (int i = 0; i < 10 && !released; ++i)
        {
            const auto next = pair();
            expect(next["door_state"] == "OPEN", "release restarted animation");
            released = next["door_command"] == "OPEN";
        }
        expect(released, "HOLD release missing");
        first.close();
        read(second);
        second.close();
        Client reconnected(context, 8080);
        expect(read(reconnected)["door_state"] == "OPEN", "reconnection reset simulation");
        reconnected.close();
    }

    // Product process must run the risk-clear scenario on loopback:8080.
    void testDoorRiskProcess(TestContext& context)
    {
        Client first(context, 8080), second(context, 8080);
        const auto read = [&](Client& client)
        {
            const auto text = client.read("risk-process.snapshot");
            expect(elevsafe::isSnapshot(text), "invalid risk-process snapshot");
            return nlohmann::json::parse(text);
        };
        auto a = read(first), b = read(second);
        for (int i = 0; a != b && i < 20; ++i)
        {
            if (a["timestamp_us"] < b["timestamp_us"]) a = read(first); else b = read(second);
        }
        expect(a == b, "risk-process clients not aligned");
        bool opening = false, held = false, cleared = false, closing = false, closed = false;
        int closeEvents = 0, samples = 0;
        std::uint64_t previous = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
        while (std::chrono::steady_clock::now() < deadline)
        {
            expect(a == b, "risk-process broadcast mismatch");
            const auto us = a["timestamp_us"].get<std::uint64_t>();
            expect(samples == 0 || us > previous, "risk-process timestamp did not advance");
            previous = us; ++samples;
            if (a["door_state"] == "OPENING") opening = true;
            if (a["door_command"] == "HOLD")
            {
                held = true;
                first.send("{\"door_command\":null}");
                second.send("{\"door_command\":null}");
            }
            if (a["passengers"].empty())
            {
                cleared = true;
                expect(us >= 7500000, "risk cleared before scripted boundary");
            }
            else
            {
                expect(a["passengers"][0]["state"] == "ENTERING"
                    && a["passengers"][0]["safety_state"] == "FALL", "synthetic passenger changed");
            }
            if (a["door_command"] == "CLOSE") ++closeEvents;
            if (a["door_state"] == "CLOSING")
            {
                closing = true;
                expect(cleared && us >= 11500000, "risk close before clear + hold");
            }
            if (closing && a["door_state"] == "CLOSED") { closed = true; break; }
            a = read(first); b = read(second);
        }
        expect(opening && held && cleared && closing && closed && closeEvents == 1,
            "missing risk process state/event");
        first.close(); second.close();
        std::cout << "risk-clear process PASS clients=2 samples=" << samples
                  << " closeEvents=" << closeEvents << " finalTimestampUs=" << previous << '\n';
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
    if (selected == "door-risk-process") return runTest("door-risk-process", testDoorRiskProcess) ? 0 : 1;
    if (selected == "door-sim-process") return runTest("door-sim-process", testDoorSimProcess) ? 0 : 1;
    if (selected == "mock-process") return runTest("mock-process", testMockProcess) ? 0 : 1;
    if (selected == "null-compatibility") return runTest("null-compatibility", testNullCompatibility) ? 0 : 1;
    if (selected == "door-risk-scenarios") return runTest("door-risk-scenarios", testDoorRiskScenarios) ? 0 : 1;
    if (selected == "mock-scenarios") return runTest("mock-scenarios", testMockScenarios) ? 0 : 1;
    const Case cases[] = {
        {"null-compatibility", testNullCompatibility},
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
