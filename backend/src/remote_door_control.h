#pragma once

#include "elevsafe/websocket_server.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <random>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace elevsafe
{
    // Accessed under WebSocketServer's command mutex. No socket or door ownership.
    class RemoteDoorControl
    {
    public:
        using Json = nlohmann::json;
        using Steady = std::chrono::steady_clock;
        using Wall = std::chrono::system_clock;

        explicit RemoteDoorControl(unsigned ttlMs) : ttl_(ttlMs) {}

        bool claim(std::uint64_t client)
        {
            if (client_) return false;
            client_ = client;
            return true;
        }

        void disconnect(std::uint64_t client)
        {
            if (client_ != client) return;
            client_.reset();
            link_.clear();
            seen_.clear();
            pending_.reset();
            releasePriority();
        }

        bool hasPriority() const { return !owner_.empty(); }
        void releasePriority() { owner_.clear(); }

        std::optional<std::uint64_t> boundClient() const
        {
            return link_.empty() ? std::nullopt : client_;
        }

        bool update(bool active, std::string_view reason)
        {
            if (active != active_)
            {
                if (active)
                {
                    if (epoch_.empty())
                    {
                        std::random_device random;
                        for (int i = 0; i < 4; ++i) epoch_ += std::to_string(random()) + "-";
                    }
                    session_ = epoch_ + std::to_string(++generation_);
                }
                else
                {
                    session_.clear();
                    releasePriority();
                }
                active_ = active;
            }
            const bool changed = reason_ != reason || publishedSession_ != session_;
            publishedSession_ = session_;
            reason_ = reason;
            return changed;
        }

        Json readiness() const
        {
            const bool ready = active_ && reason_.empty();
            return {{"type", "control_ready"}, {"enabled", true}, {"backend_connected", true}, {"ready", ready},
                {"reason", ready ? Json(nullptr) : Json(reason_)},
                {"backend_session_id", session_.empty() ? Json(nullptr) : Json(session_)}};
        }

        // A request needs a fresh Relay validity reply before it can reach the door owner.
        std::optional<Json> receive(std::uint64_t client, std::string_view text,
            Wall::time_point wallNow = Wall::now(), Steady::time_point monoNow = Steady::now())
        {
            const auto msg = Json::parse(text, nullptr, false);
            if (client_ != client || !msg.is_object()) throw std::invalid_argument("invalid control payload");
            const auto type = msg.value("type", std::string{});
            if (type == "control_bind" && msg.size() == 2 && link_.empty())
            {
                auto link = msg.at("control_session_id").get<std::string>();
                if (!validId(link)) throw std::invalid_argument("invalid control session");
                link_ = std::move(link);
                return readiness();
            }
            if (type == "control_cancel" && msg.size() == 4)
            {
                if (pending_ && msg.at("request_id") == pending_->request.requestId &&
                    msg.at("control_session_id") == link_ &&
                    msg.at("backend_session_id") == pending_->request.backendSession)
                    return release("CANCELLED", "CONTROL_SESSION_ENDED");
                return std::nullopt;
            }
            if (type == "control_revoke" && msg.size() == 3)
            {
                const auto operatorId = msg.at("operator_id").get<std::string>();
                if (!validId(operatorId)) throw std::invalid_argument("invalid operator connection");
                if (link_.empty() || msg.at("control_session_id") != link_) return std::nullopt;
                if (owner_ == operatorId) releasePriority();
                if (pending_ && pending_->operatorId == operatorId)
                    return release("CANCELLED", "CONTROL_SESSION_ENDED");
                return std::nullopt;
            }
            if (type == "control_validity" && msg.size() == 5)
            {
                if (!pending_ || pending_->validated || !matchingRequest(msg, pending_->request))
                    return std::nullopt;
                if (auto invalid = invalidate()) return invalid;
                if (!msg.at("remaining_ms").is_number_integer())
                    throw std::invalid_argument("invalid Relay validity budget");
                const auto remaining = msg.at("remaining_ms").get<std::int64_t>();
                if (remaining < 0 || remaining > pending_->originalTtl.count())
                    throw std::invalid_argument("invalid Relay validity budget");
                // Relay sampled the budget after this challenge started. Charge the
                // entire round trip, including both hops and queueing delays.
                pending_->request.expiresAt = pending_->startedMono + std::chrono::milliseconds{remaining};
                pending_->validated = true;
                if (expired(*pending_, wallNow, monoNow)) return release("EXPIRED", "REQUEST_EXPIRED");
                return std::nullopt;
            }
            if (type != "door_request" || msg.size() != 8) throw std::invalid_argument("invalid control request");
            ManualDoorRequest request{client, msg.at("door_command").get<std::string>()};
            request.requestId = msg.at("request_id").get<std::string>();
            request.controlSession = msg.at("control_session_id").get<std::string>();
            request.backendSession = msg.at("backend_session_id").get<std::string>();
            auto operatorId = msg.at("operator_id").get<std::string>();
            if (!validId(request.requestId) || !validId(request.controlSession) ||
                !validId(request.backendSession) || !validId(operatorId))
                throw std::invalid_argument("invalid control identifiers");
            const auto reject = [&](const char* reason) { return result(request, "REJECTED", reason); };
            if (request.command != "OPEN" && request.command != "HOLD" && request.command != "CLOSE") return reject("INVALID_COMMAND");
            if (link_.empty() || request.controlSession != link_ || request.backendSession != session_ ||
                !active_) return reject("SESSION_CHANGED");
            if (!reason_.empty()) return reject(reason_.c_str());
            if (seen_.contains(request.requestId)) return reject("DUPLICATE_REQUEST");
            if (seen_.size() >= 8192) return reject("SESSION_LIMIT");
            seen_.insert(request.requestId);
            if (pending_) return reject("QUEUE_FULL");
            const auto issued = parseUtc(msg.at("issued_at").get<std::string>());
            const auto expires = parseUtc(msg.at("expires_at").get<std::string>());
            if (!issued || !expires || *expires <= *issued || *expires - *issued > ttl_)
                return reject("INVALID_DEADLINE");
            const auto originalTtl = *expires - *issued;
            request.expiresAt = monoNow + originalTtl; // Bound the wait if Relay never replies.
            Json challenge{{"type", "control_validate"}, {"request_id", request.requestId},
                {"control_session_id", request.controlSession}, {"backend_session_id", request.backendSession}};
            pending_ = Pending{std::move(request), std::move(operatorId), originalTtl, monoNow, wallNow};
            return challenge;
        }

        std::optional<Json> invalidate()
        {
            if (!pending_ || (active_ && reason_.empty())) return std::nullopt;
            if (!active_) return release("CANCELLED", "SESSION_CHANGED");
            return release("REJECTED", reason_.c_str());
        }

        bool apply(const std::function<bool(const ManualDoorRequest&, bool)>& consume, std::optional<Json>& reply,
            Wall::time_point wallNow = Wall::now(), Steady::time_point monoNow = Steady::now())
        {
            if (!pending_) return false;
            if (reply = invalidate(); reply) return false;
            if (expired(*pending_, wallNow, monoNow))
            {
                reply = release("EXPIRED", "REQUEST_EXPIRED");
                return false;
            }
            if (!pending_->validated) return false;
            const bool applied = consume(pending_->request, owner_ == pending_->operatorId);
            if (applied) owner_ = std::move(pending_->operatorId);
            reply = applied ? release("APPLIED", nullptr) : release("REJECTED", "ALREADY_IN_STATE");
            return applied;
        }

        static bool validToken(std::string_view token)
        {
            return token.size() >= 32 && token.size() <= 256 &&
                std::all_of(token.begin(), token.end(), idChar);
        }

        static bool authorized(std::string_view header, std::string_view expected)
        {
            if (!header.starts_with("Bearer ") || header.size() != expected.size() + 7) return false;
            unsigned difference = 0;
            for (std::size_t i = 0; i < expected.size(); ++i)
                difference |= static_cast<unsigned char>(header[i + 7]) ^ static_cast<unsigned char>(expected[i]);
            return difference == 0;
        }

    private:
        using Utc = std::chrono::sys_time<std::chrono::milliseconds>;
        struct Pending
        {
            ManualDoorRequest request;
            std::string operatorId;
            std::chrono::milliseconds originalTtl;
            Steady::time_point startedMono;
            Wall::time_point startedWall;
            bool validated = false;
        };

        static bool matchingRequest(const Json& msg, const ManualDoorRequest& request)
        {
            return msg.at("request_id") == request.requestId && msg.at("control_session_id") == request.controlSession &&
                msg.at("backend_session_id") == request.backendSession;
        }

        static bool expired(const Pending& pending, Wall::time_point wall, Steady::time_point mono)
        {
            // Absolute UTC offsets are immaterial. A forward step can shorten the
            // budget; a backward step never extends the monotonic deadline.
            return mono >= pending.request.expiresAt ||
                wall - pending.startedWall >= pending.request.expiresAt - pending.startedMono;
        }

        static bool idChar(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        }

        static bool validId(std::string_view id)
        {
            return !id.empty() && id.size() <= 80 && std::all_of(id.begin(), id.end(), idChar);
        }

        static std::optional<Utc> parseUtc(std::string_view text)
        {
            using namespace std::chrono;
            if (text.size() != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
                text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z')
                return std::nullopt;
            auto field = [text](std::size_t start, std::size_t length)
            {
                auto segment = text.substr(start, length);
                if (!std::all_of(segment.begin(), segment.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return -1;
                int value{};
                const auto parsed = std::from_chars(segment.data(), segment.data() + segment.size(), value);
                return parsed.ec == std::errc{} ? value : -1;
            };
            const auto y = field(0, 4), m = field(5, 2), d = field(8, 2);
            const auto h = field(11, 2), min = field(14, 2), s = field(17, 2), ms = field(20, 3);
            const auto date = year{y}/month{static_cast<unsigned>(m)}/day{static_cast<unsigned>(d)};
            if (y < 2000 || y > 2100 || m < 1 || d < 1 || !date.ok() ||
                h < 0 || h > 23 || min < 0 || min > 59 || s < 0 || s > 59 || ms < 0)
                return std::nullopt;
            return sys_days{date} + hours{h} + minutes{min} + seconds{s} + milliseconds{ms};
        }

        static Json result(const ManualDoorRequest& request, const char* status, const char* reason)
        {
            return {{"type", "door_result"}, {"request_id", request.requestId},
                {"control_session_id", request.controlSession}, {"backend_session_id", request.backendSession},
                {"door_command", request.command}, {"status", status},
                {"reason", reason ? Json(reason) : Json(nullptr)}};
        }

        Json release(const char* status, const char* reason)
        {
            auto reply = result(pending_->request, status, reason);
            pending_.reset();
            return reply;
        }

        const std::chrono::milliseconds ttl_;
        std::optional<std::uint64_t> client_;
        std::optional<Pending> pending_;
        std::unordered_set<std::string> seen_;
        std::string link_, epoch_, session_, publishedSession_, owner_, reason_ = "BACKEND_UNAVAILABLE";
        std::uint64_t generation_ = 0;
        bool active_ = false;
    };
}
