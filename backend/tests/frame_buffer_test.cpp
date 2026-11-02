#include "elevsafe/frame_buffer.h"

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

    elevsafe::RadarFrame makeFrame(std::uint32_t frameCount, std::uint64_t timestampUs = 0)
    {
        elevsafe::RadarFrame frame;
        frame.frameCount = frameCount;
        frame.timestampUs = timestampUs;

        elevsafe::RadarPoint point;
        point.x = 1.25f;
        point.y = -2.5f;
        point.z = 3.75f;
        point.doppler = -4.5f;
        point.power = 5.125f;
        point.targetId = 42;
        frame.points.push_back(point);
        return frame;
    }

    void testConstruction()
    {
        elevsafe::FrameBuffer buffer(3);
        expect(buffer.empty(), "new buffer must be empty");
        expect(buffer.size() == 0, "new buffer must have size zero");
        expect(buffer.capacity() == 3, "unexpected buffer capacity");
    }

    void testPushPopAndContent()
    {
        elevsafe::FrameBuffer buffer(2);
        buffer.push(makeFrame(7, 40605));

        expect(buffer.size() == 1, "push must increase buffer size");
        const auto frame = buffer.pop();
        expect(frame.has_value(), "pop must return a frame");
        expect(frame->frameCount == 7, "frame count was not preserved");
        expect(frame->timestampUs == 40605, "frame timestamp was not preserved");
        expect(frame->points.size() == 1, "point count was not preserved");
        expect(frame->points[0].x == 1.25f, "x was not preserved");
        expect(frame->points[0].y == -2.5f, "y was not preserved");
        expect(frame->points[0].z == 3.75f, "z was not preserved");
        expect(frame->points[0].doppler == -4.5f, "doppler was not preserved");
        expect(frame->points[0].power == 5.125f, "power was not preserved");
        expect(frame->points[0].targetId == 42, "target id was not preserved");
        expect(buffer.empty(), "buffer must be empty after pop");
    }

    void testFrameCountGapPreservesTimestamps()
    {
        elevsafe::FrameBuffer buffer(2);
        buffer.push(makeFrame(100, 0));
        buffer.push(makeFrame(105, 50000));

        const auto first = buffer.pop();
        expect(first.has_value(), "gap test first pop must return a frame");
        expect(first->frameCount == 100, "gap test changed the first frame count");
        expect(first->timestampUs == 0, "gap test changed the first timestamp");

        const auto second = buffer.pop();
        expect(second.has_value(), "gap test second pop must return a frame");
        expect(second->frameCount == 105, "gap test changed the second frame count");
        expect(second->timestampUs == 50000, "frame count gap must not alter the timestamp");
    }

    void testFifo()
    {
        elevsafe::FrameBuffer buffer(3);
        buffer.push(makeFrame(1));
        buffer.push(makeFrame(2));
        buffer.push(makeFrame(3));

        const auto first = buffer.pop();
        expect(first.has_value(), "FIFO pop 1 must return a frame");
        expect(first->frameCount == 1, "FIFO order mismatch for frame 1");

        const auto second = buffer.pop();
        expect(second.has_value(), "FIFO pop 2 must return a frame");
        expect(second->frameCount == 2, "FIFO order mismatch for frame 2");

        const auto third = buffer.pop();
        expect(third.has_value(), "FIFO pop 3 must return a frame");
        expect(third->frameCount == 3, "FIFO order mismatch for frame 3");
    }

    void testOverflowDropsOldest()
    {
        elevsafe::FrameBuffer buffer(3);
        buffer.push(makeFrame(1));
        buffer.push(makeFrame(2));
        buffer.push(makeFrame(3));
        buffer.push(makeFrame(4));

        expect(buffer.size() == 3, "overflow must keep capacity bound");
        const auto first = buffer.pop();
        expect(first.has_value(), "overflow pop 1 must return a frame");
        expect(first->frameCount == 2, "oldest frame was not dropped");

        const auto second = buffer.pop();
        expect(second.has_value(), "overflow pop 2 must return a frame");
        expect(second->frameCount == 3, "unexpected second overflow frame");

        const auto third = buffer.pop();
        expect(third.has_value(), "overflow pop 3 must return a frame");
        expect(third->frameCount == 4, "unexpected newest overflow frame");
        expect(buffer.empty(), "buffer must be empty after overflow frames are popped");
    }

    void testRepeatedOverflowKeepsBound()
    {
        elevsafe::FrameBuffer buffer(3);
        for (std::uint32_t frameCount = 1; frameCount <= 100; ++frameCount)
        {
            buffer.push(makeFrame(frameCount));
            expect(buffer.size() <= buffer.capacity(), "buffer exceeded its capacity");
        }

        const auto first = buffer.pop();
        expect(first.has_value(), "final FIFO pop 1 must return a frame");
        expect(first->frameCount == 98, "final FIFO oldest frame mismatch");

        const auto second = buffer.pop();
        expect(second.has_value(), "final FIFO pop 2 must return a frame");
        expect(second->frameCount == 99, "final FIFO middle frame mismatch");

        const auto third = buffer.pop();
        expect(third.has_value(), "final FIFO pop 3 must return a frame");
        expect(third->frameCount == 100, "final FIFO newest frame mismatch");
        expect(buffer.empty(), "final FIFO validation must consume the buffer");
    }

    void testEmptyPop()
    {
        elevsafe::FrameBuffer buffer(1);
        expect(!buffer.pop().has_value(), "empty pop must return nullopt");
    }

    void testZeroCapacity()
    {
        bool threw = false;
        try
        {
            elevsafe::FrameBuffer buffer(0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }

        expect(threw, "zero capacity must throw invalid_argument");
    }

    void testClear()
    {
        elevsafe::FrameBuffer buffer(2);
        buffer.push(makeFrame(1));
        buffer.push(makeFrame(2));
        buffer.clear();

        expect(buffer.empty(), "clear must empty the buffer");
        expect(buffer.size() == 0, "clear must reset size");
        expect(buffer.capacity() == 2, "clear must preserve capacity");
    }
}

int main()
{
    try
    {
        testConstruction();
        testPushPopAndContent();
        testFrameCountGapPreservesTimestamps();
        testFifo();
        testOverflowDropsOldest();
        testRepeatedOverflowKeepsBound();
        testEmptyPop();
        testZeroCapacity();
        testClear();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "frame_buffer_test failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "frame_buffer_test passed\n";
    return 0;
}
