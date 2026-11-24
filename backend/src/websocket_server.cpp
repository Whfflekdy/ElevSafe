#include "elevsafe/websocket_server.h"
#include "elevsafe/websocket_message.h"
#include "remote_door_control.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string_view>
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
        std::optional<RemoteDoorControl> remoteControl;
        bool manualCommandsEnabled = true; // Protected by commandMutex.
        std::atomic<std::size_t> connected{0}, accepted{0}, sent{0}, ingressDrops{0}, sessionDrops{0};
        std::atomic<std::size_t> received{0}, rejected{0}, queueFull{0};
        std::atomic<std::size_t> disconnected{0}, writeTimeouts{0}, discarded{0};
        std::atomic<std::size_t> ingressPending{0}, sessionPending{0}, activeWrites{0};
        std::atomic<std::size_t> peakIngress{0}, peakPending{0}, peakActive{0};
        std::promise<void> ioFinished;
        std::future<void> ioFinishedFuture{ioFinished.get_future()};
        std::thread thread;

        Impl(WebSocketConfig value, std::ostream& output) : config(std::move(value)), log(output)
        {
            if (!config.remoteControlToken.empty()) remoteControl.emplace(config.remoteControlTtlMs);
        }
        void postControl(std::uint64_t client, RemoteDoorControl::Json payload);
        void accept();
        void deliver();
        void shutdown(bool force = false);
        void run(bool finalDrain = false);
        void failWorker(const char* message);
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
        std::deque<Message> controlOutbox;
        bool control = false;
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
                    const bool controlPath = request.target() == "/control-local";
                    const bool known = request.target() == "/radar" ||
                        (controlPath && self->owner.remoteControl.has_value());
                    if (!known) return self->rejectUpgrade(http::status::not_found, request.version());
                    if (!ws::is_upgrade(request)) return self->rejectUpgrade(http::status::bad_request, request.version());
                    if (controlPath)
                    {
                        const auto header = request[http::field::authorization];
                        if (!RemoteDoorControl::authorized({header.data(), header.size()},
                                self->owner.config.remoteControlToken))
                            return self->rejectUpgrade(http::status::forbidden, request.version());
                        std::lock_guard lock(self->owner.commandMutex);
                        if (!self->owner.remoteControl->claim(self->id))
                            return self->rejectUpgrade(http::status::conflict, request.version());
                        self->control = true;
                    }
                    beast::get_lowest_layer(self->stream).expires_never();
                    self->stream.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
                    self->stream.read_message_max(self->control
                        ? self->owner.config.maxControlBytes : self->owner.config.maxRequestBytes);
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

        void rejectUpgrade(http::status status, unsigned version)
        {
            auto response = std::make_shared<http::response<http::string_body>>(status, version);
            response->set(http::field::content_type, "text/plain");
            response->body() = status == http::status::forbidden || status == http::status::conflict
                ? "WebSocket upgrade rejected\n" : "WebSocket endpoint is /radar\n";
            response->prepare_payload();
            http::async_write(stream.next_layer(), *response,
                [self = shared_from_this(), response](Error error, std::size_t) { self->finish("HTTP rejected", error); });
        }

        void finish(const char* where, Error error = {})
        {
            if (finished) return;
            finished = true;
            if (control)
            {
                std::lock_guard lock(owner.commandMutex);
                owner.remoteControl->disconnect(id);
                controlOutbox.clear();
            }
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
            }
            owner.sessions.erase(id);
            // Diagnostics must not interrupt cancellation, including the forced drain.
            try
            {
                if (ready) owner.writeLog("disconnected client=" + std::to_string(id) + " reason=" + where);
                if (error && error != net::error::operation_aborted && error != ws::error::closed)
                    owner.writeLog(std::string(where) + " client=" + std::to_string(id)
                        + " code=" + std::to_string(error.value()) + " category=" + error.category().name()
                        + " message=" + error.message());
            }
            catch (...) {}
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
                self->input.consume(self->input.size());
                if (self->control)
                {
                    if (!self->stream.got_text()) return self->finish("invalid control payload");
                    std::optional<RemoteDoorControl::Json> reply;
                    try
                    {
                        std::lock_guard lock(self->owner.commandMutex);
                        reply = self->owner.remoteControl->receive(self->id, text);
                    }
                    catch (const std::invalid_argument&) { return self->finish("invalid control payload"); }
                    catch (const RemoteDoorControl::Json::exception&) { return self->finish("invalid control payload"); }
                    if (reply) self->offerControl(std::make_shared<const std::string>(reply->dump()));
                    self->read();
                    return;
                }
                const auto command = self->stream.got_text() ? parseDoorRequest(text) : std::nullopt;
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
                    bool enabled;
                    {
                        std::lock_guard lock(self->owner.commandMutex);
                        const auto& control = self->owner.remoteControl;
                        enabled = self->owner.manualCommandsEnabled && !(control && control->hasPriority());
                        full = enabled && self->owner.commands.size() >= self->owner.config.maxCommands;
                        if (enabled && !full) self->owner.commands.push_back({self->id, *command});
                    }
                    if (full)
                    {
                        ++self->owner.queueFull;
                        self->owner.writeLog("command queue full; disconnecting client=" + std::to_string(self->id));
                        return self->finish("command queue full");
                    }
                    if (enabled)
                    {
                        ++self->owner.received;
                        self->owner.writeLog("manual request received client=" + std::to_string(self->id)
                            + " command=" + *command + " (not executed; no ACK)");
                    }
                    else
                    {
                        ++self->owner.rejected;
                        self->owner.writeLog("manual request ignored: no active command window or remote priority");
                    }
                }
                self->read();
            });
        }

        void offerControl(Message message)
        {
            if (!ready || stopping || finished || !control) return;
            if (message->size() > owner.config.maxControlBytes || controlOutbox.size() >= 8)
                return finish("control output limit");
            if (active)
            {
                controlOutbox.push_back(std::move(message));
                return;
            }
            pending = std::move(message);
            const auto count = ++owner.sessionPending;
            owner.peakPending.store(std::max(owner.peakPending.load(), count));
            write();
        }

        void offer(Message message)
        {
            if (!ready || stopping || finished || control) return;
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
                    if (!self->control) ++self->owner.sent;
                    if (self->finished) return;
                    if (self->stopping) return self->closeHandshake();
                    if (!self->controlOutbox.empty())
                    {
                        auto next = std::move(self->controlOutbox.front());
                        self->controlOutbox.pop_front();
                        self->offerControl(std::move(next));
                    }
                    else if (self->pending) self->write();
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

    void WebSocketServer::Impl::postControl(std::uint64_t client, RemoteDoorControl::Json payload)
    {
        if (!running) return;
        auto message = std::make_shared<const std::string>(payload.dump());
        net::post(io, [this, client, message = std::move(message)]
        {
            if (!running) return;
            const auto session = sessions.find(client);
            if (session == sessions.end()) return;
            auto current = session->second;
            current->offerControl(message);
        });
    }

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
        {
            std::lock_guard lock(commandMutex);
            manualCommandsEnabled = false;
            commands.clear();
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

    void WebSocketServer::Impl::failWorker(const char* message)
    {
        running = false;
        shutdown(true);
        try { writeLog(std::string("I/O handler failed; canceled pending work: ") + message); }
        catch (...) {} // Preserve teardown even if the diagnostic stream fails.
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
            catch (const std::exception& error) { failWorker(error.what()); }
            catch (...) { failWorker("unknown exception"); }
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
            if (state.remoteControl && (!endpoint.address().is_loopback() ||
                !RemoteDoorControl::validToken(state.config.remoteControlToken) ||
                state.config.remoteControlTtlMs <= 250 || state.config.remoteControlTtlMs > 10000 ||
                state.config.maxControlBytes < 512 || state.config.maxControlBytes > 4096))
                throw std::invalid_argument("Invalid loopback control configuration");
            Error error;
            const int backlog = static_cast<int>(std::min<std::size_t>(state.config.maxClients, 128));
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
                state.acceptor.listen(backlog, error);
            }
            if (error)
            {
                // Preserve the original failure even if querying the bound endpoint fails.
                Error endpointError;
                const auto bound = state.acceptor.local_endpoint(endpointError);
                Error ignored;
                state.acceptor.close(ignored);
                throw boost::system::system_error(error, std::string("WebSocket ") + stage + " failed: "
                    + state.config.address + ':' + std::to_string(state.config.port)
                    + " family=" + (endpoint.address().is_v4() ? "IPv4" : "IPv6")
                    + " address_length=" + std::to_string(endpoint.size())
                    + " bound_port=" + (endpointError ? std::string("unavailable") : std::to_string(bound.port()))
                    + (std::string_view(stage) == "listen" ? " backlog=" + std::to_string(backlog) : ""));
            }
            state.boundPort = state.acceptor.local_endpoint(error).port();
            if (error)
                throw boost::system::system_error(error, "WebSocket local_endpoint failed: "
                    + state.config.address + ':' + std::to_string(state.config.port));
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
        const auto reportStop = [&state](const char* phase, bool includeWork, long long elapsedMs = -1) noexcept
        {
            try
            {
                std::string message(phase);
                if (elapsedMs >= 0) message += " elapsedMs=" + std::to_string(elapsedMs);
                if (includeWork) message += state.pendingWork();
                state.writeLog(message);
            }
            catch (...) {} // Logging must not prevent join or cleanup.
        };
        reportStop("stop.request", true);
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
                reportStop("stop.thread.wait", true, elapsed);
            }
            state.thread.join();
            reportStop("stop.thread.joined", false);
        }
        // Also covers failed startup and an I/O thread that already exited.
        state.io.restart();
        state.shutdown(true);
        state.run(true);
        reportStop("stop.complete", true);
    }

    bool WebSocketServer::publishSnapshot(std::string message)
    {
        auto& state = *m_impl;
        if (!state.running || message.size() > state.config.maxSnapshotBytes || !isSnapshot(message)) return false;
        auto owned = std::make_shared<const std::string>(std::move(message));
        std::lock_guard lock(state.ingressMutex);
        if (!state.running) return false;
        // Post before committing the slot. The handler must acquire ingressMutex,
        // so a failed post leaves no undeliverable snapshot or stuck notification.
        if (!state.notificationPending)
        {
            net::post(state.io, [&state] { state.deliver(); });
            state.notificationPending = true;
        }
        if (state.latest) ++state.ingressDrops;
        state.latest = std::move(owned);
        state.ingressPending = 1;
        state.peakIngress = 1;
        return true;
    }

    void WebSocketServer::setManualCommandsEnabled(bool enabled)
    {
        std::size_t discarded = 0;
        bool changed;
        {
            std::lock_guard lock(m_impl->commandMutex);
            changed = m_impl->manualCommandsEnabled != enabled;
            m_impl->manualCommandsEnabled = enabled;
            if (!enabled)
            {
                if (m_impl->remoteControl) m_impl->remoteControl->releasePriority();
                discarded = m_impl->commands.size();
                m_impl->commands.clear();
            }
        }
        if (changed || discarded)
        {
            try
            {
                m_impl->writeLog(std::string("manual command window=") + (enabled ? "open" : "closed")
                    + " discarded=" + std::to_string(discarded));
            }
            catch (...) {} // Queue state is already committed.
        }
    }

    void WebSocketServer::updateRemoteDoor(bool active, std::string_view reason)
    {
        auto& state = *m_impl;
        if (!state.remoteControl) return;
        std::lock_guard lock(state.commandMutex);
        auto& control = *state.remoteControl;
        if (!control.update(active, reason)) return;
        const auto client = control.boundClient();
        if (!client) return;
        if (auto cancelled = control.invalidate()) state.postControl(*client, std::move(*cancelled));
        state.postControl(*client, control.readiness());
    }

    bool WebSocketServer::applyRemoteDoor(const std::function<bool(const ManualDoorRequest&, bool)>& consume,
        bool motionFinished)
    {
        auto& state = *m_impl;
        if (!state.remoteControl) return false;
        std::lock_guard lock(state.commandMutex);
        auto& control = *state.remoteControl;
        if (motionFinished) control.releasePriority();
        const auto client = control.boundClient();
        std::optional<RemoteDoorControl::Json> reply;
        const bool applied = client && control.apply(consume, reply);
        if (control.hasPriority()) state.commands.clear();
        if (reply) state.postControl(*client, std::move(*reply));
        return applied;
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
