#include "elevsafe/websocket_server.h"
#include "elevsafe/websocket_message.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <syncstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace elevsafe
{
    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ws = beast::websocket;
    using Tcp = net::ip::tcp;
    using Error = boost::system::error_code;
    using Message = std::shared_ptr<const std::string>;

    struct WebSocketServer::Impl
    {
        struct Session;
        WebSocketConfig config;
        std::ostream& log;
        net::io_context io;
        net::executor_work_guard<net::io_context::executor_type> work{io.get_executor()};
        Tcp::acceptor acceptor{io};
        std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
        std::uint64_t nextId = 1;
        std::atomic<bool> running{false};
        bool started = false;
        bool shutdownStarted = false;
        std::uint16_t boundPort = 0;
        std::mutex ingressMutex;
        Message latest;
        bool notificationPending = false;
        std::mutex commandMutex;
        std::deque<ManualDoorRequest> commands;
        std::atomic<std::size_t> connected{0}, accepted{0}, sent{0}, ingressDrops{0}, sessionDrops{0};
        std::atomic<std::size_t> received{0}, rejected{0}, queueFull{0};
        std::atomic<std::size_t> disconnected{0}, writeTimeouts{0}, discarded{0};
        std::atomic<std::size_t> ingressPending{0}, sessionPending{0}, activeWrites{0};
        std::atomic<std::size_t> peakIngress{0}, peakPending{0}, peakActive{0};
        std::promise<void> ioFinished;
        std::future<void> ioFinishedFuture{ioFinished.get_future()};
        std::thread thread;

        Impl(WebSocketConfig value, std::ostream& output) : config(std::move(value)), log(output) {}
        void accept();
        void deliver();
        void shutdown(bool force = false);
        void run(bool finalDrain = false);
        std::string pendingWork() const;
        void writeLog(const std::string& text)
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::osyncstream(log) << "[ws] " << text << " timeMs=" << now
                << " thread=" << std::this_thread::get_id() << '\n';
        }
    };

    struct WebSocketServer::Impl::Session : std::enable_shared_from_this<Session>
    {
        Impl& owner;
        std::uint64_t id;
        ws::stream<beast::tcp_stream> stream;
        beast::flat_buffer input;
        http::request_parser<http::string_body> parser;
        net::steady_timer writeTimer;
        net::steady_timer closeTimer;
        Message active;
        Message pending;
        std::uint64_t writeGeneration = 0;
        bool ready = false;
        bool stopping = false;
        bool finished = false;
        bool closeStarted = false;

        Session(Impl& server, std::uint64_t client, Tcp::socket socket) :
            owner(server), id(client), stream(std::move(socket)),
            writeTimer(stream.get_executor()), closeTimer(stream.get_executor())
        {
            parser.header_limit(8192);
            parser.body_limit(0);
        }

        void start()
        {
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(3));
            http::async_read(stream.next_layer(), input, parser,
                [self = shared_from_this()](Error error, std::size_t)
                {
                    if (error) return self->finish("HTTP read", error);
                    if (self->finished) return;
                    const auto& request = self->parser.get();
                    if (request.target() != "/radar" || !ws::is_upgrade(request))
                    {
                        auto response = std::make_shared<http::response<http::string_body>>(
                            request.target() != "/radar" ? http::status::not_found : http::status::bad_request,
                            request.version());
                        response->set(http::field::content_type, "text/plain");
                        response->body() = "WebSocket endpoint is /radar\n";
                        response->prepare_payload();
                        http::async_write(self->stream.next_layer(), *response,
                            [self, response](Error error, std::size_t) { self->finish("HTTP rejected", error); });
                        return;
                    }
                    beast::get_lowest_layer(self->stream).expires_never();
                    self->stream.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
                    self->stream.read_message_max(self->owner.config.maxRequestBytes);
                    self->stream.async_accept(request, [self](Error error)
                    {
                        if (error) return self->finish("handshake", error);
                        if (self->finished) return;
                        self->ready = true;
                        ++self->owner.connected;
                        ++self->owner.accepted;
                        self->owner.writeLog("connected client=" + std::to_string(self->id));
                        self->read();
                    });
                });
        }

        void finish(const char* where, Error error = {})
        {
            if (finished) return;
            finished = true;
            Error ignored;
            writeTimer.cancel(ignored);
            closeTimer.cancel(ignored);
            beast::get_lowest_layer(stream).socket().cancel(ignored);
            beast::get_lowest_layer(stream).socket().shutdown(Tcp::socket::shutdown_both, ignored);
            beast::get_lowest_layer(stream).socket().close(ignored);
            if (pending)
            {
                pending.reset();
                --owner.sessionPending;
                ++owner.discarded;
            }
            if (ready)
            {
                --owner.connected;
                ++owner.disconnected;
                owner.writeLog("disconnected client=" + std::to_string(id) + " reason=" + where);
            }
            if (error && error != net::error::operation_aborted && error != ws::error::closed)
                owner.writeLog(std::string(where) + " client=" + std::to_string(id)
                    + " code=" + std::to_string(error.value()) + " category=" + error.category().name()
                    + " message=" + error.message());
            owner.sessions.erase(id);
        }

        void read()
        {
            stream.async_read(input, [self = shared_from_this()](Error error, std::size_t)
            {
                if (error)
                {
                    if (error == ws::error::message_too_big) ++self->owner.rejected;
                    return self->finish("read", error);
                }
                if (self->finished || self->stopping) return;
                const auto text = beast::buffers_to_string(self->input.data());
                const auto command = self->stream.got_text() ? parseDoorRequest(text) : std::nullopt;
                self->input.consume(self->input.size());
                if (!command && self->stream.got_text() && self->owner.config.allowNullDoorRequest
                    && isNullDoorRequest(text))
                {
                    // Compatibility notification only: never queue, execute, or count as a command/ACK.
                    self->owner.writeLog("null door request no-op client=" + std::to_string(self->id));
                }
                else if (!command)
                {
                    ++self->owner.rejected;
                    self->owner.writeLog("invalid manual request client=" + std::to_string(self->id));
                }
                else
                {
                    bool full = false;
                    {
                        std::lock_guard lock(self->owner.commandMutex);
                        full = self->owner.commands.size() >= self->owner.config.maxCommands;
                        if (!full) self->owner.commands.push_back({self->id, *command});
                    }
                    if (full)
                    {
                        ++self->owner.queueFull;
                        self->owner.writeLog("command queue full; disconnecting client=" + std::to_string(self->id));
                        return self->finish("command queue full");
                    }
                    ++self->owner.received;
                    self->owner.writeLog("manual request received client=" + std::to_string(self->id)
                        + " command=" + *command + " (not executed; no ACK)");
                }
                self->read();
            });
        }

        void offer(Message message)
        {
            if (!ready || stopping || finished) return;
            if (pending) ++owner.sessionDrops;
            else
            {
                const auto count = ++owner.sessionPending;
                owner.peakPending.store(std::max(owner.peakPending.load(), count));
            }
            pending = std::move(message);
            if (!active) write();
        }

        void write()
        {
            active = std::move(pending);
            --owner.sessionPending;
            const auto count = ++owner.activeWrites;
            owner.peakActive.store(std::max(owner.peakActive.load(), count));
            try
            {
                const auto generation = ++writeGeneration;
                writeTimer.expires_after(std::chrono::seconds(5));
                writeTimer.async_wait([self = shared_from_this(), generation](Error error)
                {
                    if (!error && self->active && generation == self->writeGeneration)
                    {
                        ++self->owner.writeTimeouts;
                        self->finish("write deadline", net::error::timed_out);
                    }
                });
                stream.text(true);
                stream.async_write(net::buffer(*active), [self = shared_from_this()](Error error, std::size_t)
                {
                    Error ignored;
                    self->writeTimer.cancel(ignored);
                    // active must stay alive until this completion, including after socket cancellation.
                    self->active.reset();
                    --self->owner.activeWrites;
                    if (error) ++self->owner.discarded;
                    if (error) return self->finish("write", error);
                    ++self->owner.sent;
                    if (self->finished) return;
                    if (self->stopping) return self->closeHandshake();
                    if (self->pending) self->write();
                });
            }
            catch (...)
            {
                active.reset();
                --owner.activeWrites;
                ++owner.discarded;
                throw;
            }
        }

        void closeHandshake()
        {
            if (closeStarted || finished) return;
            closeStarted = true;
            stream.async_close(ws::close_code::normal,
                [self = shared_from_this()](Error error) { self->finish("close", error); });
        }

        void shutdown()
        {
            stopping = true;
            if (pending)
            {
                pending.reset();
                --owner.sessionPending;
                ++owner.discarded;
            }
            if (!ready) return finish("shutdown before handshake");
            closeTimer.expires_after(std::chrono::milliseconds(500));
            closeTimer.async_wait([self = shared_from_this()](Error error)
            {
                if (!error) self->finish("shutdown deadline");
            });
            if (!active) closeHandshake();
        }
    };

    void WebSocketServer::Impl::accept()
    {
        acceptor.async_accept([this](Error error, Tcp::socket socket)
        {
            if (!running) return;
            if (!error)
            {
                if (sessions.size() < config.maxClients)
                {
                    const auto id = nextId++;
                    auto session = std::make_shared<Session>(*this, id, std::move(socket));
                    sessions.emplace(id, session);
                    session->start();
                }
                else
                {
                    Error ignored;
                    socket.close(ignored);
                    writeLog("client limit reached");
                }
            }
            else if (error != net::error::operation_aborted)
                writeLog("accept failed: " + error.message());
            accept();
        });
    }

    void WebSocketServer::Impl::deliver()
    {
        Message message;
        {
            std::lock_guard lock(ingressMutex);
            message = std::move(latest);
            ingressPending = 0;
            notificationPending = false;
        }
        if (!running || !message) return;
        for (const auto& item : sessions) item.second->offer(message);
    }

    void WebSocketServer::Impl::shutdown(bool force)
    {
        if (shutdownStarted && !force) return;
        shutdownStarted = true;
        running = false;
        {
            std::lock_guard lock(ingressMutex);
            latest.reset();
            ingressPending = 0;
            notificationPending = false;
        }
        Error ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        work.reset();
        // finish() erases its own entry; retain the session and advance first.
        for (auto iterator = sessions.begin(); iterator != sessions.end();)
        {
            auto session = (iterator++)->second;
            if (force) session->finish("forced shutdown");
            else session->shutdown();
        }
    }

    std::string WebSocketServer::Impl::pendingWork() const
    {
        return " connected=" + std::to_string(connected.load())
            + " ingressPending=" + std::to_string(ingressPending.load())
            + " sessionPending=" + std::to_string(sessionPending.load())
            + " activeWrites=" + std::to_string(activeWrites.load());
    }

    void WebSocketServer::Impl::run(bool finalDrain)
    {
        auto nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;)
        {
            try
            {
                if (!finalDrain) { io.run(); return; }
                while (!io.stopped())
                {
                    io.run_for(std::chrono::milliseconds(250));
                    if (!io.stopped() && std::chrono::steady_clock::now() >= nextReport)
                    {
                        writeLog("stop.drain.wait pending cancellation completions" + pendingWork());
                        nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    }
                }
                return;
            }
            catch (const std::exception& error)
            {
                running = false;
                writeLog(std::string("I/O handler failed; canceling pending work: ") + error.what());
                shutdown(true);
            }
            catch (...)
            {
                running = false;
                writeLog("Unknown I/O handler exception; canceling pending work");
                shutdown(true);
            }
            // Asio permits re-entering run() after a handler throws. Only drain
            // canceled work here: running is false, so no connections restart.
        }
    }

    WebSocketServer::WebSocketServer(WebSocketConfig config, std::ostream& log) :
        m_impl(std::make_unique<Impl>(std::move(config), log)) {}

    WebSocketServer::~WebSocketServer() { stop(); }

    void WebSocketServer::start()
    {
        auto& state = *m_impl;
        if (state.started) throw std::logic_error("WebSocketServer is one-shot; create a new server to restart");
        state.started = true;
        try
        {
            if (state.config.maxClients == 0 || state.config.maxCommands == 0
                || state.config.maxSnapshotBytes == 0 || state.config.maxRequestBytes == 0)
                throw std::invalid_argument("WebSocket limits must be positive");
            const Tcp::endpoint endpoint(net::ip::make_address(state.config.address), state.config.port);
            Error error;
            const char* stage = "open";
            state.acceptor.open(endpoint.protocol(), error);
            if (!error)
            {
                stage = "bind";
                state.acceptor.bind(endpoint, error);
            }
            if (!error)
            {
                stage = "listen";
                // A finite backlog matches our client bound and avoids provider-specific SOMAXCONN handling.
                state.acceptor.listen(static_cast<int>(std::min<std::size_t>(state.config.maxClients, 128)), error);
            }
            if (error)
            {
                Error ignored;
                state.acceptor.close(ignored);
                throw boost::system::system_error(error, std::string("WebSocket ") + stage + " failed: "
                    + state.config.address + ':' + std::to_string(state.config.port));
            }
            state.boundPort = state.acceptor.local_endpoint().port();
            state.running = true;
            state.accept();
            state.thread = std::thread([&state]
            {
                state.run();
                state.ioFinished.set_value_at_thread_exit();
            });
            state.writeLog("listening ws://" + state.config.address + ':' + std::to_string(state.boundPort) + "/radar");
        }
        catch (...)
        {
            stop();
            throw;
        }
    }

    void WebSocketServer::stop() noexcept
    {
        auto& state = *m_impl;
        if (!state.started) return;
        if (!state.thread.joinable() && state.shutdownStarted && state.io.stopped()) return;
        state.writeLog("stop.request" + state.pendingWork());
        {
            std::lock_guard lock(state.ingressMutex);
            state.running = false;
            state.latest.reset();
            state.ingressPending = 0;
        }
        if (state.thread.joinable())
        {
            try { net::post(state.io, [&state] { state.shutdown(); }); }
            catch (...) { state.io.stop(); } // Clean synchronously after joining if posting fails.
            const auto start = std::chrono::steady_clock::now();
            while (state.ioFinishedFuture.wait_for(std::chrono::seconds(1)) != std::future_status::ready)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                state.writeLog("stop.thread.wait elapsedMs=" + std::to_string(elapsed) + state.pendingWork());
            }
            state.thread.join();
            state.writeLog("stop.thread.joined");
        }
        // Also covers failed startup and an I/O thread that already exited.
        state.io.restart();
        state.shutdown(true);
        state.run(true);
        state.writeLog("stop.complete" + state.pendingWork());
    }

    bool WebSocketServer::publishSnapshot(std::string message)
    {
        auto& state = *m_impl;
        if (message.size() > state.config.maxSnapshotBytes || !isSnapshot(message)) return false;
        auto owned = std::make_shared<const std::string>(std::move(message));
        std::lock_guard lock(state.ingressMutex);
        if (!state.running) return false;
        if (state.latest) ++state.ingressDrops;
        state.latest = std::move(owned);
        state.ingressPending = 1;
        state.peakIngress = 1;
        if (!state.notificationPending)
        {
            state.notificationPending = true;
            net::post(state.io, [&state] { state.deliver(); });
        }
        return true;
    }

    std::optional<ManualDoorRequest> WebSocketServer::popCommand()
    {
        std::lock_guard lock(m_impl->commandMutex);
        if (m_impl->commands.empty()) return std::nullopt;
        auto request = std::move(m_impl->commands.front());
        m_impl->commands.pop_front();
        return request;
    }

    WebSocketStatistics WebSocketServer::statistics() const noexcept
    {
        const auto& s = *m_impl;
        return {s.connected.load(), s.accepted.load(), s.sent.load(), s.ingressDrops.load(),
            s.sessionDrops.load(), s.received.load(), s.rejected.load(), s.queueFull.load(),
            s.disconnected.load(), s.writeTimeouts.load(), s.discarded.load(), s.ingressPending.load(),
            s.sessionPending.load(), s.activeWrites.load(), s.peakIngress.load(), s.peakPending.load(),
            s.peakActive.load()};
    }

    std::uint16_t WebSocketServer::port() const noexcept { return m_impl->boundPort; }
}
