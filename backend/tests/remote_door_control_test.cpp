#include "../src/remote_door_control.h"

#include <iostream>
#include <stdexcept>

using elevsafe::ManualDoorRequest;
using elevsafe::RemoteDoorControl;
using Json = RemoteDoorControl::Json;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace
{
    const auto utc = sys_days{2026y / November / 24} + 4h;
    const auto mono = RemoteDoorControl::Steady::time_point{};
    constexpr auto client = 42u;

    void expect(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct Scenario
    {
        RemoteDoorControl control{2000};
        Json session;
        std::optional<Json> result;
        unsigned applied = 0;

        Scenario()
        {
            expect(control.claim(client) && !control.claim(43), "one authenticated peer");
            expect(bind("link-1")->at("ready") == false, "waiting for first observation");
            control.update(true, "DOOR_UNKNOWN");
            expect(control.readiness()["reason"] == "DOOR_UNKNOWN", "unknown door is not eligible");
            control.update(true, "");
            session = control.readiness()["backend_session_id"];
        }

        std::optional<Json> send(const Json& msg, milliseconds t = 0ms, milliseconds wallOffset = 0ms)
        {
            return control.receive(client, msg.dump(), utc + t + wallOffset, mono + t);
        }

        std::optional<Json> bind(const char* id)
        {
            return send({{"type", "control_bind"}, {"control_session_id", id}});
        }

        Json request(const char* id, const char* owner = "operator-1") const
        {
            return {{"type", "door_request"}, {"door_command", "HOLD"}, {"operator_id", owner},
                {"control_session_id", "link-1"}, {"backend_session_id", session}, {"request_id", id},
                {"issued_at", "2026-11-24T04:00:00.000Z"}, {"expires_at", "2026-11-24T04:00:02.000Z"}};
        }

        static Json validity(Json challenge, milliseconds remaining)
        {
            challenge["type"] = "control_validity";
            challenge["remaining_ms"] = remaining.count();
            return challenge;
        }

        bool tick(milliseconds t, milliseconds wallOffset = 0ms, bool accept = true, bool sameOwner = false)
        {
            result.reset();
            return control.apply([&](const ManualDoorRequest& item, bool same)
            {
                expect(item.command == "HOLD" && item.backendSession == session.get<std::string>(), "wrong request routed");
                expect(same == sameOwner, "wrong ownership at application");
                ++applied;
                return accept;
            }, result, utc + t + wallOffset, mono + t);
        }
    };

    void timing(const char* name, milliseconds start, milliseconds sampled, milliseconds arrived,
        milliseconds applyAt, milliseconds wallOffset, bool accepted, milliseconds wallStep = 0ms)
    {
        Scenario s;
        const auto challenge = s.send(s.request("timing"), start, wallOffset);
        expect(challenge && challenge->at("type") == "control_validate", "missing expiry challenge");
        expect(!s.tick(start, wallOffset) && !s.result && s.applied == 0, "applied without Relay validity");
        const auto proofResult = s.send(Scenario::validity(*challenge, std::max(0ms, 2000ms - sampled)), arrived, wallOffset);
        const bool applied = s.tick(applyAt, wallOffset + wallStep);
        expect(applied == accepted && s.applied == (accepted ? 1u : 0u), name);
        const auto& terminal = proofResult ? proofResult : s.result;
        expect(terminal && terminal->at("status") == (accepted ? "APPLIED" : "EXPIRED"), name);
    }
}

int main()
try
{
    timing("normal no clock skew", 0ms, 0ms, 0ms, 1ms, 0ms, true);
    timing("normal TTL with network delay", 10ms, 70ms, 170ms, 1850ms, 0ms, true);
    timing("exact TTL boundary", 0ms, 0ms, 0ms, 2000ms, 0ms, false);
    timing("past TTL", 0ms, 0ms, 0ms, 2001ms, 0ms, false);
    timing("regression: Relay to Bridge -200ms, Bridge to A -200ms, no callback at Relay 2100ms",
        300ms, 400ms, 500ms, 2100ms, -400ms, false);
    timing("opposite clock offsets still allow fresh requests", 300ms, 400ms, 500ms, 1600ms, 400ms, true);
    timing("opposite clock offsets cannot extend TTL", 300ms, 400ms, 500ms, 2100ms, 400ms, false);
    timing("asymmetric return delay exhausts remaining budget", 1000ms, 1010ms, 1995ms, 1995ms, 0ms, false);
    timing("FIFO wait late", 100ms, 200ms, 300ms, 1900ms, 0ms, false);
    timing("UTC backward cannot extend deadline", 100ms, 200ms, 300ms, 1900ms, 0ms, false, -40s);
    timing("UTC forward expires pending request", 100ms, 200ms, 300ms, 400ms, 0ms, false, 2s);
    timing("Relay already expired before challenge", 2010ms, 2020ms, 2040ms, 2050ms, 0ms, false);

    Scenario s;
    auto request = s.request("first");
    const auto challenge = *s.send(request);
    expect(s.send(request)->at("reason") == "DUPLICATE_REQUEST", "duplicate fenced");
    expect(s.send(s.request("over-limit"))->at("reason") == "QUEUE_FULL", "one pending command");
    auto wrongGrant = Scenario::validity(challenge, 2s);
    wrongGrant["request_id"] = "other";
    expect(!s.send(wrongGrant) && !s.tick(10ms), "another request cannot authorize apply");
    expect(!s.send(Scenario::validity(challenge, 1800ms), 15ms), "valid grant queued");
    expect(s.tick(25ms) && s.result->at("status") == "APPLIED" && s.control.hasPriority(), "first owner applied");

    const auto accept = [&](const char* id, const char* owner)
    {
        s.send(Scenario::validity(*s.send(s.request(id, owner)), 2s));
    };
    accept("not-applied", "operator-2");
    expect(!s.tick(25ms, 0ms, false) && s.result->at("status") == "REJECTED", "rejected request");
    const auto revoke = [&](const char* id)
    {
        return s.send({{"type", "control_revoke"}, {"control_session_id", "link-1"}, {"operator_id", id}});
    };
    revoke("operator-2");
    expect(s.control.hasPriority(), "rejection and unrelated disconnect preserve owner");
    accept("repeat", "operator-1");
    expect(!s.tick(25ms, 0ms, false, true) && s.control.hasPriority(), "same owner repeat preserves ownership");
    const auto cancelledOnly = *s.send(s.request("cancel-only"));
    const auto cancelMessage = Json{{"type", "control_cancel"}, {"request_id", "cancel-only"},
        {"control_session_id", "link-1"}, {"backend_session_id", s.session}};
    expect(s.send(cancelMessage)->at("status") == "CANCELLED" && s.control.hasPriority() &&
        !s.send(Scenario::validity(cancelledOnly, 2s)), "request cancellation does not revoke owner");
    const auto cancel = *s.send(s.request("cancel"));
    expect(revoke("operator-1")->at("status") == "CANCELLED" &&
        !s.control.hasPriority() && !s.send(Scenario::validity(cancel, 2s)) && !s.tick(25ms),
        "operator revoke cancels before apply and clears ownership");
    accept("new-owner", "operator-3");
    expect(s.tick(25ms) && s.control.hasPriority(), "new operator can apply");

    s.send(s.request("stale"));
    s.control.update(true, "RADAR_STALE");
    expect(s.control.invalidate()->at("reason") == "RADAR_STALE", "stale Radar invalidates pending");
    s.control.update(true, "");
    s.send(s.request("radar-end"));
    s.control.update(false, "BACKEND_UNAVAILABLE");
    expect(s.control.invalidate()->at("status") == "CANCELLED" && !s.control.hasPriority(), "Radar EOF cancels pending");
    s.control.update(true, "");
    expect(s.control.readiness()["backend_session_id"] != s.session &&
        s.send(request)->at("reason") == "SESSION_CHANGED", "old Radar session fenced");
    s.session = s.control.readiness()["backend_session_id"];
    accept("after-radar", "operator-1");
    expect(s.tick(25ms) && s.control.hasPriority(), "new Radar session accepted");
    const auto oldProof = Scenario::validity(*s.send(s.request("reconnect")), 2s);
    s.control.disconnect(client);
    expect(!s.control.hasPriority() && s.control.claim(client) && s.bind("link-2")->at("ready") == true,
        "control disconnect clears ownership");
    expect(!s.send(oldProof) && !s.tick(0ms) &&
        s.send(s.request("after-control"))->at("reason") == "SESSION_CHANGED", "no replay after reconnect");

    auto draft = s.request("bad-deadline");
    draft["control_session_id"] = "link-2";
    draft["expires_at"] = "2026-11-24T04:00:03.000Z";
    expect(s.send(draft)->at("reason") == "INVALID_DEADLINE", "TTL configuration limit");
    std::cout << "remote_door_control_test passed: 12 deterministic TTL cases, ownership and session fencing\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
