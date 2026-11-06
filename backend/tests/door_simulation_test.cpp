#include "elevsafe/door_simulation.h"
#include <iostream>
#include <stdexcept>

using D = elevsafe::DoorSimulation;
using namespace std::chrono_literals;
void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try
    {
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
        std::cout << "door_simulation_test passed (virtual clock, no sleeps)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
