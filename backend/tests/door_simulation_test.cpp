#include "elevsafe/door_simulation.h"
#include "elevsafe/mock_door_policy.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using D = elevsafe::DoorSimulation;
using namespace std::chrono_literals;
void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void testMockPolicy()
{
    using P = elevsafe::MockDoorPolicy;
    using J = nlohmann::json;
    const D::Time z{};
    const auto clear = J::array();
    const auto risk = J::array({elevsafe::mockDoorPassenger(1, "ENTERING", "FALL")});
    for (const auto* safety : {"FALL", "IMMOBILE"})
    {
        D inside; P p;
        p.accept(J::array({elevsafe::mockDoorPassenger(1, "INSIDE", safety)}));
        inside.apply(D::Command::Open, z);
        p.tick(inside, z + 2680ms);
        expect(inside.openHoldDeadline() == z + 6680ms, "INSIDE changed hold deadline");
        p.tick(inside, z + 6680ms);
        expect(inside.state() == D::State::Closing, "INSIDE prevented normal close");
        p.tick(inside, z + 9030ms);
        expect(inside.state() == D::State::Closed, "INSIDE cycle incomplete");
        for (const auto* state : {"ENTERING", "EXITING"})
        {
            D door; P policy;
            expect(policy.accept(J::array({elevsafe::mockDoorPassenger(1, state, safety)})), "valid risk rejected");
            policy.tick(door, z);
            expect(door.takeAppliedCommand() == D::Command::Open, "risk did not apply AUTO OPEN");
            policy.tick(door, z + 2679ms);
            expect(door.deadline() == z + 2680ms && !door.takeAppliedCommand(), "risk restarted opening");
            policy.tick(door, z + 2680ms);
            expect(door.state() == D::State::Open && door.takeAppliedCommand() == D::Command::Hold,
                "risk must HOLD at exact open completion");
            policy.tick(door, z + 20s);
            expect(door.state() == D::State::Open && !door.takeAppliedCommand(), "duplicate risk event");
            expect(!policy.accept(std::nullopt) && policy.inputMissing(), "missing input accepted");
            policy.tick(door, z + 1h);
            expect(door.state() == D::State::Open && door.activeCommand() == D::Command::Hold,
                "missing input released risk HOLD");
            auto invalid = J::array({elevsafe::mockDoorPassenger(1, "OUTSIDE", safety)});
            expect(!policy.accept(invalid), "OUTSIDE hazard accepted");
            expect(policy.doorwayRisk() && policy.passengers()[0]["state"] == state, "invalid input replaced last valid");
            policy.accept(clear);
            policy.tick(door, z + 1h + 1s);
            expect(!door.takeAppliedCommand() && door.openHoldDeadline() == z + 1h + 5s, "clear emitted command or wrong timer");
            policy.accept(clear); policy.tick(door, z + 1h + 4999ms);
            expect(door.state() == D::State::Open, "clear hold closed early");
            policy.tick(door, z + 1h + 5s);
            expect(door.takeAppliedCommand() == D::Command::Close, "clear failed automatic close");
            policy.tick(door, z + 1h + 7350ms);
            expect(door.state() == D::State::Closed && !door.takeAppliedCommand(), "clear completion event");
        }
    }
    D lost; P lostPolicy; lostPolicy.accept(risk); lostPolicy.tick(lost, z);
    lostPolicy.tick(lost, z + 2680ms); lostPolicy.accept(std::nullopt);
    lost.apply(D::Command::Close, z + 3s); lostPolicy.tick(lost, z + 5350ms);
    expect(lost.state() == D::State::Closed && !lost.activeCommand(),
        "invalid retained input issued new AUTO after manual completion");
    lostPolicy.accept(risk); lostPolicy.tick(lost, z + 6s);
    expect(lost.state() == D::State::Opening, "restored valid risk was not reevaluated");

    // Clear during movement preserves deadline and starts hold only when fully open.
    D early; P earlyPolicy; earlyPolicy.accept(risk); earlyPolicy.tick(early, z);
    earlyPolicy.accept(clear); earlyPolicy.tick(early, z + 1s);
    expect(early.deadline() == z + 2680ms, "clear restarted opening");
    earlyPolicy.tick(early, z + 2680ms);
    expect(early.openHoldDeadline() == z + 6680ms, "clear during opening hold timing");
    earlyPolicy.tick(early, z + 6680ms); const auto closeDeadline = early.deadline();
    earlyPolicy.accept(clear); earlyPolicy.tick(early, z + 7s);
    expect(early.deadline() == closeDeadline, "clear restarted closing");
    earlyPolicy.tick(early, z + 9030ms); earlyPolicy.accept(clear); earlyPolicy.tick(early, z + 10s);
    expect(early.state() == D::State::Closed, "clear reopened closed door");

    for (const auto command : {D::Command::Open, D::Command::Close, D::Command::Hold})
    {
        D door; P policy; policy.accept(risk); policy.tick(door, z); policy.tick(door, z + 2680ms);
        door.apply(command, z + 3s); policy.tick(door, z + 3s);
        expect(door.takeAppliedCommand() == command, "AUTO replaced same-tick MANUAL");
        if (command == D::Command::Close)
        {
            policy.tick(door, z + 5349ms);
            expect(door.state() == D::State::Closing, "risk interrupted manual CLOSE");
            policy.tick(door, z + 5350ms);
            expect(door.state() == D::State::Opening && door.mode() == D::Mode::Auto,
                "persistent risk not reevaluated at manual completion");
        }
        if (command == D::Command::Hold)
        {
            policy.accept(clear); policy.tick(door, z + 24h);
            expect(door.mode() == D::Mode::Manual && door.activeCommand() == command,
                "risk clear released manual HOLD");
        }
    }
    // Manual OPEN while not yet open must retain its completion deadline.
    D moving; P movingPolicy; movingPolicy.accept(risk);
    moving.apply(D::Command::Open, z); movingPolicy.tick(moving, z + 2679ms);
    expect(moving.mode() == D::Mode::Manual && moving.deadline() == z + 2680ms, "risk replaced manual OPEN");
    movingPolicy.tick(moving, z + 2680ms);
    expect(moving.mode() == D::Mode::Auto && moving.activeCommand() == D::Command::Hold, "manual OPEN did not return AUTO");

    D unknown(D::State::Unknown); P unknownPolicy; unknownPolicy.accept(risk);
    unknownPolicy.tick(unknown, z + 1h);
    expect(unknown.state() == D::State::Unknown && !unknown.takeAppliedCommand(), "risk moved UNKNOWN");
    unknown.apply(D::Command::Open, z + 1h); unknownPolicy.tick(unknown, z + 1h + 2680ms);
    expect(unknown.state() == D::State::Open && unknown.activeCommand() == D::Command::Hold, "UNKNOWN manual recovery");
    expect(!elevsafe::isMockDoorScenario("invalid"), "invalid scenario accepted");
    auto mixed = *elevsafe::mockDoorScenarioInput("multi", 0us);
    for (int i = 0; i < 3; ++i)
    {
        D door; P policy; expect(policy.accept(mixed), "mixed input invalid"); policy.tick(door, z);
        expect(door.takeAppliedCommand() == D::Command::Open, "passenger order changed result");
        expect(policy.passengers() == mixed, "passenger data changed");
        std::rotate(mixed.begin(), mixed.begin() + 1, mixed.end());
    }
    P invalidPolicy;
    for (const auto& invalid : {J(nullptr), J::object(), J::array({J::object()})})
        expect(!invalidPolicy.accept(invalid), "malformed mock input accepted");
    expect(invalidPolicy.accept(J::array({elevsafe::mockDoorPassenger(3, "OUTSIDE", nullptr)})), "OUTSIDE null rejected");
    expect(!invalidPolicy.doorwayRisk(), "OUTSIDE null created risk");
    for (const auto name : elevsafe::mockDoorScenarioNames)
    {
        P policy; expect(policy.accept(elevsafe::mockDoorScenarioInput(name, 0us)), "invalid scenario sample");
    }
    expect(!elevsafe::mockDoorScenarioInput("input-loss", 8s), "loss script not missing");
    expect(elevsafe::mockDoorScenarioInput("risk-clear", 8s)->empty(), "clear script not explicit");
    std::cout << "mock policy: risk matrix, clear/loss, order, manual and UNKNOWN passed\n";
}

int main()
{
    try
    {
        testMockPolicy();
        D d;
        const D::Time zero{};
        expect(d.state() == D::State::Closed && d.mode() == D::Mode::Auto, "initial state");
        d.apply(D::Command::Close, zero);
        expect(!d.deadline() && d.mode() == D::Mode::Auto, "already closed");
        d.apply(D::Command::Open, zero);
        expect(d.state() == D::State::Opening && d.mode() == D::Mode::Manual, "opening");
        expect(d.startedAt() == zero && d.deadline() == zero + 2680ms, "open timing");
        expect(!d.apply(D::Command::Open, zero + 1s), "repeat applied");
        expect(d.deadline() == zero + 2680ms, "repeat restarted timer");
        expect(!d.update(zero + 2680ms - 1us) && d.state() == D::State::Opening, "early open");
        expect(d.update(zero + 2680ms) && d.state() == D::State::Open
            && d.mode() == D::Mode::Auto && !d.activeCommand(), "open completion");
        d.apply(D::Command::Open, zero + 3s);
        expect(!d.deadline() && d.mode() == D::Mode::Auto, "already open");
        d.apply(D::Command::Hold, zero + 3s);
        expect(d.state() == D::State::Open && d.mode() == D::Mode::Manual, "hold open");
        d.update(zero + 1h);
        expect(d.mode() == D::Mode::Manual, "hold expired");
        d.apply(D::Command::Close, zero + 1h);
        expect(d.state() == D::State::Closing && d.activeCommand() == D::Command::Close, "hold release close");
        expect(!d.update(zero + 1h + 2350ms - 1us), "early close");
        expect(d.update(zero + 1h + 2350ms) && d.state() == D::State::Closed
            && d.mode() == D::Mode::Auto, "close completion");

        D hold;
        hold.apply(D::Command::Hold, zero);
        expect(hold.state() == D::State::Opening, "hold from closed");
        expect(!hold.apply(D::Command::Hold, zero + 1s) && hold.deadline() == zero + 2680ms, "repeat hold");
        hold.update(zero + 2680ms);
        expect(hold.state() == D::State::Open && hold.mode() == D::Mode::Manual, "hold completion");
        hold.apply(D::Command::Open, zero + 3s);
        expect(hold.mode() == D::Mode::Auto && !hold.deadline(), "hold release open");

        D conflict;
        conflict.apply(D::Command::Open, zero);
        conflict.apply(D::Command::Close, zero + 1s);
        expect(conflict.state() == D::State::Closing && conflict.deadline() == zero + 3350ms, "reverse close");
        expect(!conflict.apply(D::Command::Close, zero + 1100ms), "duplicate close");
        conflict.apply(D::Command::Open, zero + 2s);
        expect(conflict.state() == D::State::Opening && conflict.deadline() == zero + 4680ms, "reverse open");
        conflict.apply(D::Command::Hold, zero + 2s);
        conflict.apply(D::Command::Close, zero + 2s);
        expect(conflict.activeCommand() == D::Command::Close && conflict.deadline() == zero + 4350ms, "ordered burst");
        conflict.update(zero + 24h);
        expect(conflict.state() == D::State::Closed && conflict.mode() == D::Mode::Auto, "large time jump");
        expect(!conflict.update(zero + 48h), "auto started motion");
        D boundary;
        boundary.apply(D::Command::Open, zero);
        boundary.apply(D::Command::Close, zero + 2680ms);
        expect(boundary.deadline() == zero + 5030ms, "command on completion boundary");
        // Exact automatic cycle boundaries, including one-shot command publication.
        D cycle;
        cycle.apply(D::Command::Open, zero);
        expect(cycle.takeAppliedCommand() == D::Command::Open, "missing manual event");
        cycle.update(zero + 2679ms);
        expect(cycle.state() == D::State::Opening, "2679 opening");
        cycle.update(zero + 2680ms);
        expect(cycle.state() == D::State::Open && cycle.openHoldDeadline() == zero + 6680ms,
            "hold must start at fully open");
        expect(!cycle.takeAppliedCommand(), "completion emitted ACK");
        cycle.apply(D::Command::Open, zero + 3s);
        expect(cycle.openHoldDeadline() == zero + 6680ms, "already-open request extended timer");
        cycle.takeAppliedCommand();
        cycle.update(zero + 6679ms);
        expect(cycle.state() == D::State::Open, "6679 open");
        cycle.update(zero + 6680ms);
        expect(cycle.state() == D::State::Closing && cycle.mode() == D::Mode::Auto
            && cycle.deadline() == zero + 9030ms, "automatic close timing/mode");
        expect(cycle.takeAppliedCommand() == D::Command::Close, "missing automatic close event");
        for (int ms = 6700; ms <= 9000; ms += 100)
        {
            cycle.update(zero + std::chrono::milliseconds(ms));
            expect(!cycle.takeAppliedCommand(), "automatic close event repeated at 10Hz");
        }
        cycle.update(zero + 9029ms);
        expect(cycle.state() == D::State::Closing, "9029 closing");
        cycle.update(zero + 9030ms);
        expect(cycle.state() == D::State::Closed && cycle.mode() == D::Mode::Auto,
            "9030 closed");
        expect(!cycle.takeAppliedCommand(), "closed completion emitted ACK");
        cycle.update(zero + 24h);
        expect(cycle.state() == D::State::Closed, "periodic automatic opening");

        for (const auto manual : {D::Command::Hold, D::Command::Close})
        {
            D priority;
            priority.apply(D::Command::Open, zero);
            priority.update(zero + 2680ms);
            priority.takeAppliedCommand();
            priority.apply(manual, zero + 6680ms);
            priority.update(zero + 6680ms);
            expect(priority.mode() == D::Mode::Manual && priority.activeCommand() == manual,
                "manual lost to timer on same tick");
            expect(priority.takeAppliedCommand() == manual && !priority.takeAppliedCommand(),
                "extra auto event on manual boundary");
            if (manual == D::Command::Hold)
            {
                priority.update(zero + 24h);
                expect(priority.state() == D::State::Open && !priority.openHoldDeadline(),
                    "manual HOLD expired");
                expect(!priority.applyAuto(D::Command::Close, zero + 24h), "AUTO released manual HOLD");
                priority.apply(D::Command::Open, zero + 24h);
                expect(priority.openHoldDeadline() == zero + 24h + 4s, "HOLD release OPEN timer");
            }
        }

        D automatic;
        automatic.apply(D::Command::Open, zero);
        expect(!automatic.applyAuto(D::Command::Close, zero + 1s), "AUTO interrupted manual motion");
        automatic.update(zero + 2680ms);
        expect(automatic.applyAuto(D::Command::Hold, zero + 6680ms), "AUTO HOLD rejected");
        automatic.update(zero + 6680ms);
        expect(automatic.state() == D::State::Open && automatic.mode() == D::Mode::Auto,
            "AUTO HOLD lost to default timer");
        automatic.takeAppliedCommand();
        for (int i = 0; i < 100; ++i)
        {
            const auto now = zero + 7s + std::chrono::milliseconds(i * 100);
            expect(!automatic.applyAuto(D::Command::Hold, now), "duplicate AUTO HOLD");
            expect(!automatic.applyAuto(std::nullopt, now), "no input released hold");
            automatic.update(now);
            expect(automatic.state() == D::State::Open && !automatic.takeAppliedCommand(),
                "AUTO HOLD/time/no-input regression");
        }
        automatic.apply(D::Command::Close, zero + 20s);
        expect(automatic.mode() == D::Mode::Manual && automatic.state() == D::State::Closing,
            "manual close did not override AUTO HOLD");
        expect(!automatic.applyAuto(D::Command::Hold, zero + 20s), "same-tick auto overrode manual");
        automatic.update(zero + 22350ms);
        expect(automatic.state() == D::State::Closed && automatic.mode() == D::Mode::Auto,
            "manual close completion");
        automatic.update(zero + 30s);
        expect(automatic.state() == D::State::Closed, "replayed rejected AUTO input");

        D autoOpen;
        expect(autoOpen.applyAuto(D::Command::Open, zero), "AUTO OPEN rejected");
        const auto autoDeadline = autoOpen.deadline();
        autoOpen.takeAppliedCommand();
        for (int i = 1; i <= 100; ++i)
        {
            const auto now = zero + std::chrono::milliseconds(i * 100);
            expect(!autoOpen.applyAuto(D::Command::Open, now), "repeated AUTO OPEN restarted");
            autoOpen.update(now);
            expect(!autoOpen.takeAppliedCommand(), "repeated AUTO OPEN event");
            if (i < 27) expect(autoOpen.deadline() == autoDeadline, "AUTO movement deadline changed");
        }
        expect(autoOpen.state() == D::State::Open && !autoOpen.openHoldDeadline(),
            "AUTO OPEN did not inhibit normal close");
        expect(autoOpen.applyAuto(D::Command::Close, zero + 11s), "explicit AUTO CLOSE rejected");
        autoOpen.update(zero + 13350ms);
        expect(autoOpen.state() == D::State::Closed && autoOpen.mode() == D::Mode::Auto,
            "AUTO CLOSE completion");

        for (const auto manual : {D::Command::Open, D::Command::Hold, D::Command::Close})
        {
            D unknown(D::State::Unknown);
            expect(D::name(unknown.state()) == "UNKNOWN", "UNKNOWN converted to CLOSED");
            expect(!unknown.applyAuto(D::Command::Close, zero), "UNKNOWN AUTO CLOSE");
            expect(!unknown.applyAuto(D::Command::Open, zero), "UNKNOWN AUTO output should be absent");
            unknown.update(zero + 1h);
            expect(unknown.state() == D::State::Unknown && !unknown.takeAppliedCommand(),
                "UNKNOWN timer generated motion");
            expect(unknown.apply(manual, zero + 1h), "UNKNOWN blocked manual");
            unknown.update(zero + 1h + (manual == D::Command::Close ? D::closeDuration : D::openDuration));
            expect(unknown.state() == (manual == D::Command::Close ? D::State::Closed : D::State::Open),
                "manual simulation from UNKNOWN failed");
        }
        D delayed;
        delayed.apply(D::Command::Open, zero);
        delayed.takeAppliedCommand();
        delayed.update(zero + 10s);
        expect(delayed.state() == D::State::Closed && delayed.takeAppliedCommand() == D::Command::Close,
            "delayed tick did not preserve scheduled deadlines");

        std::cout << "door_simulation_test passed (virtual clock, no sleeps)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
