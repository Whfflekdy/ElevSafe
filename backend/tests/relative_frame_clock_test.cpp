#include "elevsafe/relative_frame_clock.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
    void expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void testRelativeTimestamps()
    {
        elevsafe::RelativeFrameClock clock;
        const elevsafe::RelativeFrameClock::TimePoint t0{};

        const std::uint64_t first = clock.timestampUs(t0);
        const std::uint64_t second = clock.timestampUs(t0 + std::chrono::microseconds(40605));
        const std::uint64_t third = clock.timestampUs(t0 + std::chrono::microseconds(102178));

        expect(first == 0, "first frame timestamp must be zero");
        expect(second == 40605, "second frame timestamp mismatch");
        expect(third == 102178, "third frame timestamp mismatch");
        expect(first <= second && second <= third, "frame timestamps must be non-decreasing");
    }

    void testResetStartsAtZero()
    {
        elevsafe::RelativeFrameClock clock;
        const elevsafe::RelativeFrameClock::TimePoint t0{};

        clock.timestampUs(t0);
        clock.timestampUs(t0 + std::chrono::microseconds(40605));
        clock.reset();

        expect(clock.timestampUs(t0 + std::chrono::microseconds(102178)) == 0,
            "first frame after reset must have timestamp zero");
    }
}

int main()
{
    try
    {
        testRelativeTimestamps();
        testResetStartsAtZero();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "relative_frame_clock_test failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "relative_frame_clock_test passed\n";
    return 0;
}
