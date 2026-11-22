#include "elevsafe/tracking_adapter.h"
#include "elevsafe/emulator_door_arbitration.h"
#include "elevsafe/signal_adapter.h"
#include "elevsafe/websocket_message.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void expect(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

    void testEmulatorAutoCycle()
    {
        using namespace std::chrono_literals;
        using Door = elevsafe::DoorSimulation;
        using Arb = elevsafe::EmulatorDoorArbitration;
        using Cmd = elevsafe::DoorCommand;
        using State = elevsafe::DoorState;
        const Door::Time zero{};
        Door door;
        expect(!Arb(door).applyFrame(Cmd::NONE, 0, zero, zero, false, State::CLOSED)
            && door.state() == Door::State::Closed, "disabled cycle opened the door");

        bool arrival = true;
        Arb arb(door, &arrival);
        const auto frame = [&](std::chrono::milliseconds at, Cmd command = Cmd::NONE,
            State evaluated = State::OPEN)
        {
            return arb.applyFrame(command, at.count() * 1000, zero + at, zero + at, false, evaluated);
        };
        arb.advanceMotion(zero);
        expect(arrival && !door.takeAppliedCommand(), "idle consumed the arrival");
        expect(!arb.applyFrame(Cmd::NONE, 0, zero, zero + 1001ms, false, State::CLOSED)
            && arrival, "stale frame consumed the arrival");
        expect(frame(0ms, Cmd::NONE, State::CLOSED) && !arrival
            && door.state() == Door::State::Opening && door.takeAppliedCommand() == Door::Command::Open,
            "first fresh frame did not apply AUTO OPEN");
        arb.advanceMotion(zero + 2679ms);
        expect(door.state() == Door::State::Opening, "AUTO OPEN completed early");
        arb.advanceMotion(zero + 2700ms);
        expect(door.state() == Door::State::Open, "AUTO OPEN did not complete");
        for (auto at = 3000ms; at <= 6500ms; at += 500ms)
            expect(!frame(at), "normal CLOSE occurred before the dwell elapsed");
        expect(!frame(6699ms) && !frame(6700ms, Cmd::NONE, State::UNKNOWN),
            "normal CLOSE ignored the dwell or C's UNKNOWN observation");
        expect(!arb.applyFrame(Cmd::NONE, 6750000, zero + 6750ms, zero + 7751ms, false, State::OPEN),
            "stale C result triggered normal CLOSE");
        expect(!frame(7800ms) && !frame(8300ms) && !frame(8799ms),
            "normal CLOSE bypassed the pause recovery gate");
        expect(frame(8800ms) && door.state() == Door::State::Closing
            && door.takeAppliedCommand() == Door::Command::Close, "fresh OPEN/NONE did not close normally");
        expect(!frame(8900ms) && !door.takeAppliedCommand(), "normal CLOSE produced a duplicate event");
        arb.advanceMotion(zero + 11150ms);
        expect(door.state() == Door::State::Closed && !frame(11200ms, Cmd::NONE, State::CLOSED),
            "the same session replayed its arrival");
        Door reconnected;
        expect(!Arb(reconnected, &arrival).applyFrame(Cmd::NONE, 0, zero, zero, false, State::CLOSED),
            "reconnecting replayed the consumed arrival");

        Door occupied;
        bool occupiedArrival = true;
        Arb occupiedArb(occupied, &occupiedArrival);
        const auto occupiedFrame = [&](std::chrono::milliseconds at, Cmd command)
        {
            return occupiedArb.applyFrame(command, at.count() * 1000, zero + at, zero + at, false,
                occupiedArb.trackingDoorState());
        };
        expect(occupiedFrame(0ms, Cmd::NONE) && occupied.takeAppliedCommand() == Door::Command::Open,
            "occupied scenario did not open");
        occupiedArb.advanceMotion(zero + 2680ms);
        for (auto at = 3000ms; at <= 6500ms; at += 500ms)
            expect(!occupiedFrame(at, Cmd::NONE), "occupied scenario closed too early");
        expect(occupiedFrame(6680ms, Cmd::HOLD) && occupied.takeAppliedCommand() == Door::Command::Hold
            && occupied.state() == Door::State::Open, "C HOLD did not preempt the normal CLOSE deadline");
        expect(!occupiedFrame(7000ms, Cmd::HOLD) && !occupiedFrame(7500ms, Cmd::NONE)
            && !occupied.takeAppliedCommand(), "C HOLD let the normal timer resume or emitted a duplicate");
        expect(occupiedFrame(8000ms, Cmd::CLOSE) && occupied.takeAppliedCommand() == Door::Command::Close
            && occupied.state() == Door::State::Closing, "fresh C CLOSE did not close the held door");
        expect(occupiedFrame(8100ms, Cmd::HOLD) && occupied.takeAppliedCommand() == Door::Command::Hold
            && occupied.state() == Door::State::Opening, "C HOLD did not reverse AUTO CLOSING");

        Door manual;
        bool manualArrival = true;
        Arb manualArb(manual, &manualArrival);
        expect(manualArb.applyManual(Door::Command::Hold, zero) && !manualArrival
            && manual.takeAppliedCommand() == Door::Command::Hold, "manual HOLD did not cancel pending arrival");
        manualArb.advanceMotion(zero + 2680ms);
        expect(!manualArb.applyFrame(Cmd::CLOSE, 10000000, zero + 10s, zero + 10s, false, State::OPEN)
            && manual.mode() == Door::Mode::Manual && manual.state() == Door::State::Open,
            "manual HOLD expired or was overridden");
        expect(manualArb.applyManual(Door::Command::Close, zero + 10s)
            && manual.takeAppliedCommand() == Door::Command::Close, "manual CLOSE did not release manual HOLD");
        expect(!manualArb.applyFrame(Cmd::HOLD, 10100000, zero + 10100ms, zero + 10100ms, false, State::UNKNOWN)
            && manual.state() == Door::State::Closing, "C HOLD reversed manual CLOSING");
        manualArb.advanceMotion(zero + 12350ms);
        expect(manual.mode() == Door::Mode::Auto
            && !manualArb.applyFrame(Cmd::NONE, 12400000, zero + 12400ms, zero + 12400ms, false, State::CLOSED)
            && !Arb(reconnected, &manualArrival).applyFrame(Cmd::NONE, 0, zero, zero, false, State::CLOSED),
            "manual completion or reconnect reactivated a canceled arrival");
    }

    void testEmulatorDoorArbitration()
    {
        using namespace std::chrono_literals;
        using Door = elevsafe::DoorSimulation;
        using Arb = elevsafe::EmulatorDoorArbitration;
        using Cmd = elevsafe::DoorCommand;
        using Input = elevsafe::DoorState;
        const Door::Time zero{};
        Door door;
        Arb arb(door);
        const auto frame = [&](std::chrono::milliseconds elapsed, Cmd command, bool manual = false)
        {
            return arb.applyFrame(command, elapsed.count() * 1000, zero + elapsed, zero + elapsed, manual);
        };

        expect(arb.trackingDoorState() == Input::CLOSED && !frame(0ms, Cmd::HOLD)
            && !door.takeAppliedCommand(), "C HOLD opened an already closed virtual door");
        door.apply(Door::Command::Open, zero + 100ms);
        expect(door.takeAppliedCommand() == Door::Command::Open, "manual OPEN event");
        expect(arb.trackingDoorState() == Input::UNKNOWN && !frame(200ms, Cmd::CLOSE)
            && door.mode() == Door::Mode::Manual, "auto command interrupted manual OPEN");
        expect(door.advanceMotion(zero + 2780ms) && door.mode() == Door::Mode::Auto
            && arb.trackingDoorState() == Input::OPEN, "manual OPEN did not restore AUTO");
        expect(frame(2800ms, Cmd::HOLD) && door.takeAppliedCommand() == Door::Command::Hold,
            "fresh HOLD blocked by the close-only resume guard");
        expect(!frame(2900ms, Cmd::HOLD) && !frame(2950ms, Cmd::NONE)
            && door.activeCommand() == Door::Command::Hold && !door.takeAppliedCommand(),
            "duplicate HOLD or NONE changed the held door");

        door.apply(Door::Command::Close, zero + 3000ms);
        expect(door.takeAppliedCommand() == Door::Command::Close && arb.trackingDoorState() == Input::UNKNOWN,
            "manual CLOSE did not override auto HOLD");
        const auto manualClosingDeadline = door.deadline();
        expect(!frame(3100ms, Cmd::HOLD) && door.deadline() == manualClosingDeadline
            && !door.takeAppliedCommand(), "auto HOLD reversed manual CLOSE");
        expect(door.advanceMotion(zero + 5350ms) && door.mode() == Door::Mode::Auto, "manual CLOSE completion");

        // No new Radar frame observes CLOSED before the door is reopened.
        door.apply(Door::Command::Open, zero + 5360ms);
        expect(door.takeAppliedCommand() == Door::Command::Open, "manual reopen event");
        door.advanceMotion(zero + 8040ms);
        expect(!frame(8050ms, Cmd::CLOSE) && !frame(8150ms, Cmd::CLOSE)
            && !frame(8350ms, Cmd::CLOSE) && !door.takeAppliedCommand(),
            "old C CLOSE escaped protection before a second of fresh observations");
        expect(!frame(9050ms, Cmd::CLOSE, true) && !door.takeAppliedCommand(),
            "auto CLOSE ran in the same turn as a manual request");
        expect(!arb.applyFrame(Cmd::CLOSE, 9050000, zero + 9075ms, zero + 9075ms, false),
            "duplicate source timestamp replayed C CLOSE");
        expect(frame(9100ms, Cmd::CLOSE) && door.takeAppliedCommand() == Door::Command::Close
            && arb.trackingDoorState() == Input::OPEN, "stable fresh observations did not enable auto CLOSE");
        const auto closingDeadline = door.deadline();
        expect(!frame(9150ms, Cmd::CLOSE) && door.deadline() == closingDeadline
            && !door.takeAppliedCommand(), "duplicate auto CLOSE moved its deadline or emitted an event");
        expect(frame(9250ms, Cmd::HOLD) && door.state() == Door::State::Opening
            && door.takeAppliedCommand() == Door::Command::Hold, "fresh threshold occupancy did not reverse auto CLOSE");

        door.advanceMotion(zero + 11930ms);
        expect(!frame(11950ms, Cmd::CLOSE) && !frame(12450ms, Cmd::CLOSE)
            && frame(12950ms, Cmd::CLOSE), "auto CLOSE after re-established stable observations");
        door.takeAppliedCommand();
        door.apply(Door::Command::Open, zero + 13000ms);
        expect(door.mode() == Door::Mode::Manual && door.state() == Door::State::Opening
            && door.takeAppliedCommand() == Door::Command::Open && !frame(13100ms, Cmd::CLOSE),
            "manual OPEN did not reverse auto CLOSE");
        door.apply(Door::Command::Hold, zero + 13200ms);
        door.takeAppliedCommand();
        door.advanceMotion(zero + 16000ms);
        expect(!frame(16500ms, Cmd::CLOSE) && door.mode() == Door::Mode::Manual
            && door.activeCommand() == Door::Command::Hold && !door.takeAppliedCommand(),
            "auto CLOSE released manual HOLD");

        Door unknown(Door::State::Unknown);
        Arb unknownArb(unknown);
        expect(unknownArb.trackingDoorState() == Input::UNKNOWN
            && !unknownArb.applyFrame(Cmd::HOLD, 0, zero, zero, false)
            && !unknownArb.applyFrame(Cmd::CLOSE, 100000, zero + 100ms, zero + 100ms, false)
            && !unknown.takeAppliedCommand(), "UNKNOWN accepted an automatic command");

        Door slow;
        Arb slowArb(slow);
        slow.apply(Door::Command::Open, zero);
        slow.takeAppliedCommand();
        slow.advanceMotion(zero + 2680ms);
        expect(!slowArb.applyFrame(Cmd::HOLD, 2800000, zero + 2800ms, zero + 3801ms, false)
            && !slow.takeAppliedCommand(), "late B/C result was treated as fresh");
        expect(!slowArb.applyFrame(Cmd::CLOSE, 3900000, zero + 3900ms, zero + 3900ms, false)
            && !slowArb.applyFrame(Cmd::CLOSE, 4900000, zero + 4900ms, zero + 4900ms, false)
            && slowArb.applyFrame(Cmd::CLOSE, 4950000, zero + 4950ms, zero + 4950ms, false),
            "close requires at least three consecutive frames as well as one second");
    }
}
int main(int argc, char** argv)
{
    try
    {
        testEmulatorDoorArbitration();
        testEmulatorAutoCycle();
        using namespace std::chrono_literals;
        using Json = nlohmann::json;
        using Demo = elevsafe::TrackingDemo;
        const Demo::Clock::time_point zero{};
        const std::chrono::system_clock::time_point utc{};
        elevsafe::sp::PipelineResult b;
        b.frame_id = 19470; b.timestamp_us = 30052013;
        elevsafe::sp::Cluster c;
        c.cluster_id = 37; c.cx = .25f; c.cy = -.5f; c.cz = 1.25f;
        c.x_min = 0; c.x_max = .5f; c.y_min = -1; c.y_max = 0;
        c.z_min = .25f; c.z_max = 2.25f; c.num_points = 40; c.mean_doppler = .5f;
        c.power_sum = 100.0f;
        c.zone = elevsafe::sp::Zone::CABIN; c.z_top = 1.75f; c.maybe_merged = true;
        auto small = c; small.num_points = 39;
        b.clusters = {small, c};
        const auto fd = elevsafe::toTrackingDetections(b);
        expect(fd.frame == b.frame_id && fd.timestamp_us == b.timestamp_us, "frame/time changed");
        expect(fd.detections.size() == 1, "39/40 filter boundary");
        const auto& d = fd.detections.front();
        expect(d.cluster == 37 && d.cx == .25 && d.cy == -.5 && d.cz == 1.25,
            "centroid or cluster mapping");
        expect(d.sx == .5 && d.sy == 1 && d.sz == 2 && d.zmin == .25 && d.zmax == 2.25,
            "extent mapping");
        expect(d.n_points == 40 && d.mean_doppler == .5 && d.mean_power == 2.5, "metadata mapping");
        expect(d.z_top == 1.75 && d.maybe_merged == 1, "height or merged hint mapping");
        using elevsafe::sp::Zone;
        using elevsafe::DetZone;
        using elevsafe::sp::DoorState;
        using elevsafe::DoorCommand;
        constexpr Zone bZones[] = {Zone::NONE, Zone::DOOR, Zone::CABIN, Zone::HALL};
        constexpr DetZone cZones[] = {DetZone::UNKNOWN, DetZone::DOOR, DetZone::CABIN, DetZone::HALL};
        b.clusters.clear();
        for (std::size_t i = 0; i < 4; ++i)
        {
            expect(static_cast<int>(bZones[i]) == static_cast<int>(cZones[i]), "B/C zone enum values changed");
            auto cluster = c;
            cluster.zone = bZones[i];
            cluster.maybe_merged = false;
            b.clusters.push_back(cluster);
        }
        const auto mappedZones = elevsafe::toTrackingDetections(b);
        expect(mappedZones.detections.size() == 4, "zone mapping changed cluster count");
        for (std::size_t i = 0; i < mappedZones.detections.size(); ++i)
            expect(mappedZones.detections[i].zone == cZones[i] && mappedZones.detections[i].maybe_merged == 0,
                "zone or false merged hint mapping");

        struct PowerCase
        {
            int clusterId;
            int pointCount;
            float sum;
            double mean;
        };
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        const auto inf = std::numeric_limits<float>::infinity();
        const PowerCase powerCases[] = {
            {80, 40, nan, 0.0},
            {17, 40, 101.0f, 2.525},
            {41, 40, 0.0f, 0.0},
            {12, 50, -125.0f, -2.5},
            { 7, 40, inf, 0.0},
            {63, 40, -inf, 0.0},
        };
        b.clusters.clear();
        for (const auto& sample : powerCases)
        {
            auto cluster = c;
            cluster.cluster_id = sample.clusterId;
            cluster.num_points = sample.pointCount;
            cluster.power_sum = sample.sum;
            b.clusters.push_back(cluster);
        }
        const auto mixed = elevsafe::toTrackingDetections(b);
        expect(mixed.detections.size() == b.clusters.size(), "invalid power dropped a detection");
        for (std::size_t i = 0; i < mixed.detections.size(); ++i)
        {
            const auto& actual = mixed.detections[i];
            const auto& expected = powerCases[i];
            expect(actual.cluster == expected.clusterId && actual.n_points == expected.pointCount,
                "power mapping changed detection order or point count");
            expect(std::abs(actual.mean_power - expected.mean) < 1e-12, "incorrect or non-finite mean power");
            expect(actual.cx == d.cx && actual.cy == d.cy && actual.cz == d.cz
                && actual.sx == d.sx && actual.sy == d.sy && actual.sz == d.sz
                && actual.zmin == d.zmin && actual.zmax == d.zmax && actual.mean_doppler == d.mean_doppler,
                "power mapping changed geometry or Doppler");
        }
        b.clusters.clear();
        const auto emptyDetections = elevsafe::toTrackingDetections(b);
        expect(emptyDetections.detections.empty() && emptyDetections.frame == b.frame_id
            && emptyDetections.timestamp_us == b.timestamp_us, "empty frame or metadata changed");
        b.clusters = {c};
        Demo demo;
        auto first = demo.process(b, zero, utc);
        expect(first && Json::parse(*first)["passengers"].empty(), "tentative exposed");
        b.timestamp_us += 50000; ++b.frame_id;
        expect(!demo.process(b, zero + 50ms, utc), "50ms should not publish");
        b.timestamp_us += 50000; ++b.frame_id;
        auto confirmed = demo.process(b, zero + 100ms, utc + 1s);
        expect(confirmed && elevsafe::isSnapshot(*confirmed), "confirmed snapshot missing");
        auto json = Json::parse(*confirmed);
        expect(json["timestamp"] == "1970-01-01T00:00:01Z", "wall UTC not used");
        expect(json["timestamp_us"] == b.timestamp_us && json["door_state"] == "UNKNOWN"
            && json["door_command"].is_null(), "snapshot contract");
        expect(json["passengers"].size() == 1 && json["passengers"][0]["id"] == 1
            && json["passengers"][0]["state"] == "INSIDE", "C CABIN phase must override negative y");
        b.clusters[0].cluster_id = 99; b.timestamp_us += 100000;
        json = Json::parse(*demo.process(b, zero + 200ms, utc + 2s));
        expect(json["passengers"][0]["id"] == 1, "B cluster ID became tracking ID");
        expect(demo.processedFrames() == 4, "C skipped rate-limited input");
        b.clusters.clear(); b.timestamp_us += 100000;
        auto empty = demo.process(b, zero + 300ms, utc + 3s);
        expect(empty && Json::parse(*empty)["passengers"].empty(), "empty input did not remove LOST");
        b.clusters = {c}; b.timestamp_us = 0;
        Demo newSession;
        expect(Json::parse(*newSession.process(b, zero, utc))["passengers"].empty(), "session history leaked");
        TrackOutput inside; inside.state = TrackState::CONFIRMED; inside.id = 8; inside.y = 0;
        TrackOutput lost = inside; lost.state = TrackState::LOST;
        TrackOutput tentative = inside; tentative.state = TrackState::TENTATIVE;
        for (auto state : {elevsafe::sp::DoorState::OPEN, elevsafe::sp::DoorState::CLOSED,
                elevsafe::sp::DoorState::UNKNOWN})
        {
            b.estimated_door_state = state;
            json = Json::parse(elevsafe::trackingSnapshot(b, {inside, lost, tentative}, utc));
            expect(json["passengers"].size() == 1 && json["passengers"][0]["state"] == "INSIDE"
                && json["passengers"][0]["safety_state"].is_null()
                && json["door_state"] == elevsafe::sp::doorStateToString(state), "state/filter contract");
        }
        elevsafe::sp::PipelineResult series;
        auto sample = c;
        sample.zone = Zone::HALL;
        sample.cy = -.5f;
        sample.mean_doppler = 0.0f;
        Demo phases;
        const auto observe = [&](Demo& session, DoorState door)
        {
            series.estimated_door_state = door;
            series.clusters = {sample};
            auto result = session.observe(series);
            expect(result.frame == series.frame_id && result.timestamp_us == series.timestamp_us,
                "C result frame/time changed");
            ++series.frame_id;
            series.timestamp_us += 100000;
            return result;
        };
        FrameResult observed;
        for (int i = 0; i < 4; ++i) observed = observe(phases, DoorState::OPEN);
        expect(observed.tracks.empty(), "disabled HALL produced a C track");
        series.hall_enabled = true;
        for (int i = 0; i < 4; ++i) observed = observe(phases, DoorState::OPEN);
        expect(observed.tracks.size() == 1 && observed.tracks[0].phase == elevsafe::Phase::OUTSIDE,
            "B HALL flag did not enable C tracking");
        const auto hallId = observed.tracks[0].id;
        const auto phaseSnapshot = [&](const FrameResult& result, DoorState door)
        {
            auto frame = series;
            frame.timestamp_us = result.timestamp_us;
            frame.estimated_door_state = door;
            return Json::parse(elevsafe::trackingSnapshot(frame, result.tracks, utc));
        };
        expect(phaseSnapshot(observed, DoorState::OPEN)["passengers"][0]["state"] == "OUTSIDE",
            "C OUTSIDE not serialized");
        sample.zone = Zone::DOOR;
        sample.cy = -.4f;
        observed = observe(phases, DoorState::OPEN);
        expect(observed.door_command == DoorCommand::HOLD, "B OPEN did not reach C");
        json = phaseSnapshot(observed, DoorState::OPEN);
        expect(json["passengers"][0]["id"] == hallId && json["passengers"][0]["state"] == "ENTERING"
            && json["door_command"].is_null(), "C ENTERING or diagnostic-only HOLD policy changed");
        expect(observe(phases, DoorState::UNKNOWN).door_command == DoorCommand::NONE, "B UNKNOWN did not reach C");
        expect(observe(phases, DoorState::CLOSED).door_command == DoorCommand::NONE, "B CLOSED did not reach C");
        json = Json::parse(elevsafe::trackingSnapshot(observed.timestamp_us, observed.tracks, utc, "OPENING", "OPEN"));
        expect(json["door_state"] == "OPENING" && json["door_command"] == "OPEN"
            && json["passengers"][0]["state"] == "ENTERING",
            "Emulator snapshot must preserve virtual door and C passenger state");

        Demo safety;
        series = {};
        sample.zone = Zone::CABIN;
        sample.cy = .6f;
        const auto height = [&](float top)
        {
            sample.z_top = top;
            sample.cz = top * .6f;
            sample.z_min = .1f;
            sample.z_max = top;
            return observe(safety, DoorState::CLOSED);
        };
        for (int i = 0; i < 20; ++i) observed = height(1.7f);
        const auto safetyJson = [&](const FrameResult& result)
        {
            const auto snapshot = phaseSnapshot(result, DoorState::CLOSED);
            expect(snapshot.size() == 5 && snapshot["door_command"].is_null()
                && snapshot["passengers"].size() == 1, "C safety snapshot changed the v7 contract");
            return snapshot["passengers"][0];
        };
        auto passenger = safetyJson(observed);
        const auto safetyId = passenger["id"];
        expect(passenger["safety_state"].is_null(), "NORMAL must be JSON null");
        for (int i = 1; i <= 5; ++i) observed = height(1.7f - .28f * i);
        passenger = safetyJson(observed);
        expect(passenger["id"] == safetyId && passenger["safety_state"] == "FALL",
            "C FALL did not reach v7 with the same ID");
        for (int i = 0; i < 420; ++i) observed = height(.3f);
        passenger = safetyJson(observed);
        expect(passenger["id"] == safetyId && passenger["safety_state"] == "IMMOBILE",
            "C IMMOBILE did not reach v7 with the same ID");
        series.clusters.clear();
        observed = safety.observe(series);
        expect(observed.tracks.size() == 1 && observed.tracks[0].lost
            && observed.safety_state == elevsafe::Safety::IMMOBILE, "empty frame did not preserve C LOST safety");
        expect(phaseSnapshot(observed, DoorState::CLOSED)["passengers"].empty(), "LOST track exposed in v7");
        Demo resetSafety;
        series = {};
        for (int i = 0; i < 4; ++i) observed = observe(resetSafety, DoorState::CLOSED);
        expect(observed.tracks.size() == 1 && observed.tracks[0].id == 1
            && safetyJson(observed)["safety_state"].is_null(), "new session retained C safety or ID state");

        expect(argc == 2, "signal fixture config required");
        elevsafe::sp::Config config; expect(config.load(argv[1]), "fixture config load");
        elevsafe::sp::SignalPipeline signal(config); expect(signal.ok(), "B init");
        Demo realB;
        for (int i = 0; i < 3; ++i)
        {
            elevsafe::RadarFrame frame; frame.frameCount = i; frame.timestampUs = i * 100000;
            for (int j = 0; j < 40; ++j)
            {
                elevsafe::RadarPoint point; point.x = .25f; point.y = .8f; point.z = 1;
                point.power = 10; point.targetId = 255; frame.points.push_back(point);
            }
            auto result = signal.process(elevsafe::toSignalFrame(frame), {});
            expect(result.clusters.size() == 1 && result.clusters[0].num_points == 40, "B fixture cluster");
            expect(result.clusters[0].power_sum == 400.0f, "B fixture power sum");
            const auto fromB = elevsafe::toTrackingDetections(result);
            expect(fromB.detections.size() == 1 && fromB.detections.front().mean_power == 10.0,
                "B power sum did not reach C detection as a mean");
            auto snapshot = realB.process(result, zero + i * 100ms, utc);
            if (i == 2) expect(Json::parse(*snapshot)["passengers"].size() == 1, "A-B-C path failed");
        }
        std::cout << "tracking_adapter_test PASS: mapping, 39/40, all-frame C, door arbitration, HALL, Phase/Safety, IDs, empty, UTC, v7, session reset, real A-B-C\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
