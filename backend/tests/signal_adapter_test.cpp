#include "elevsafe/signal_adapter.h"

#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void expectOverflow(const elevsafe::RadarFrame& frame)
    {
        bool threw = false;
        try
        {
            elevsafe::toSignalFrame(frame);
        }
        catch (const std::overflow_error&)
        {
            threw = true;
        }
        expect(threw, "out-of-range metadata must throw overflow_error");
    }

    void testPointsAndSourcePreservation()
    {
        elevsafe::RadarFrame frame;
        frame.frameCount = 42;
        frame.timestampUs = 101234;
        frame.points.push_back({1.25f, -2.5f, 3.75f, -4.5f, 5.125f, -1});
        auto single = elevsafe::toSignalFrame(frame);
        expect(single.points.size() == 1, "single point was lost");

        frame.points.push_back({-0.0f, 6.25f, -7.5f, 8.75f, 9.0f, 255});
        frame.points.push_back({10.0f, 11.0f, 12.0f, -13.0f, 14.0f,
            std::numeric_limits<std::int32_t>::min()});
        frame.points.push_back({15.0f, 16.0f, 17.0f, 18.0f, 19.0f,
            std::numeric_limits<std::int32_t>::max()});
        auto converted = elevsafe::toSignalFrame(frame);
        expect(converted.frame_id == 42, "frameCount changed");
        expect(converted.timestamp_us == 101234, "integer microseconds changed");
        expect(converted.points.size() == frame.points.size(), "point count changed");
        for (std::size_t i = 0; i < frame.points.size(); ++i)
        {
            const auto& source = frame.points[i];
            const auto& point = converted.points[i];
            expect(std::bit_cast<std::uint32_t>(point.x) == std::bit_cast<std::uint32_t>(source.x), "x/order changed");
            expect(point.y == source.y && point.z == source.z, "y/z/order changed");
            expect(point.doppler == source.doppler && point.power == source.power, "doppler/power changed");
            expect(point.target_id == source.targetId, "target id changed");
            expect(point.zone == elevsafe::sp::Zone::NONE, "zone must use B default");
            expect(point.cluster_id == -1 && !point.has_bins, "B metadata must retain defaults");
            expect(point.range_bin == 0 && point.u_bin == 0 && point.v_bin == 0, "bins changed");
        }
        converted.points[0].x = 99.0f;
        expect(frame.points[0].x == 1.25f && frame.points[0].targetId == -1, "adapter modified source points");
        expect(frame.frameCount == 42 && frame.timestampUs == 101234, "adapter modified source metadata");
    }

    void testEmptyAndIntegerLimits()
    {
        elevsafe::RadarFrame frame;
        auto empty = elevsafe::toSignalFrame(frame);
        expect(empty.points.empty() && empty.frame_id == 0 && empty.timestamp_us == 0, "empty frame changed");
        frame.frameCount = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
        frame.timestampUs = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        auto boundary = elevsafe::toSignalFrame(frame);
        expect(boundary.frame_id == std::numeric_limits<int>::max(), "frame boundary changed");
        expect(boundary.timestamp_us == std::numeric_limits<std::int64_t>::max(), "timestamp boundary changed");
        frame.frameCount += 1;
        expectOverflow(frame);
        frame.frameCount = std::numeric_limits<std::uint32_t>::max();
        expectOverflow(frame);
        frame.frameCount = 0;
        frame.timestampUs += 1;
        expectOverflow(frame);
        frame.timestampUs = std::numeric_limits<std::uint64_t>::max();
        expectOverflow(frame);
    }
}

int main()
{
    try
    {
        testPointsAndSourcePreservation();
        testEmptyAndIntegerLimits();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "signal_adapter_test failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "signal_adapter_test passed\n";
    return 0;
}
