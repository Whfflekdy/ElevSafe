#include "elevsafe/backend_pipeline.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

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
        elevsafe::BackendPipeline pipeline(3);
        expect(pipeline.pendingSize() == 0, "new pipeline must have no pending frames");
    }

    void testOnFrame()
    {
        elevsafe::BackendPipeline pipeline(2);
        pipeline.onFrame(makeFrame(7));

        expect(pipeline.pendingSize() == 1, "onFrame must enqueue one frame");
    }

    void testDrainSingleFrame()
    {
        elevsafe::BackendPipeline pipeline(2);
        pipeline.onFrame(makeFrame(7, 40605));

        std::size_t callbackCount = 0;
        std::uint32_t receivedFrameCount = 0;
        std::uint64_t receivedTimestampUs = 0;
        const auto drained = pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            ++callbackCount;
            receivedFrameCount = frame.frameCount;
            receivedTimestampUs = frame.timestampUs;
            expect(frame.points.size() == 1, "point count was not preserved");
            expect(frame.points[0].x == 1.25f, "x was not preserved");
            expect(frame.points[0].y == -2.5f, "y was not preserved");
            expect(frame.points[0].z == 3.75f, "z was not preserved");
            expect(frame.points[0].doppler == -4.5f, "doppler was not preserved");
            expect(frame.points[0].power == 5.125f, "power was not preserved");
            expect(frame.points[0].targetId == 42, "target id was not preserved");
        });

        expect(drained == 1, "single-frame drain must return one");
        expect(receivedFrameCount == 7, "consumer received an unexpected frame count");
        expect(receivedTimestampUs == 40605, "consumer received an unexpected frame timestamp");
        expect(callbackCount == 1, "consumer must be called once");
        expect(pipeline.pendingSize() == 0, "drain must empty the pipeline");
    }

    void testFrameCountGapPreservesTimestamps()
    {
        elevsafe::BackendPipeline pipeline(2);
        pipeline.onFrame(makeFrame(100, 0));
        pipeline.onFrame(makeFrame(105, 50000));

        std::vector<std::uint32_t> frameCounts;
        std::vector<std::uint64_t> timestampsUs;
        const auto drained = pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            frameCounts.push_back(frame.frameCount);
            timestampsUs.push_back(frame.timestampUs);
        });

        expect(drained == 2, "gap test must drain both frames");
        expect(frameCounts.size() == 2, "gap test frame count size mismatch");
        expect(timestampsUs.size() == 2, "gap test timestamp size mismatch");
        expect(frameCounts[0] == 100 && frameCounts[1] == 105, "pipeline changed gapped frame counts");
        expect(timestampsUs[0] == 0 && timestampsUs[1] == 50000,
            "pipeline must preserve timestamps independently of frame count gaps");
    }

    void testDrainReturnsCountAndPreservesFifo()
    {
        elevsafe::BackendPipeline pipeline(3);
        pipeline.onFrame(makeFrame(1));
        pipeline.onFrame(makeFrame(2));
        pipeline.onFrame(makeFrame(3));

        std::vector<std::uint32_t> receivedFrameCounts;
        const auto drained = pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            receivedFrameCounts.push_back(frame.frameCount);
        });

        expect(drained == 3, "drain must report three processed frames");
        expect(receivedFrameCounts.size() == 3, "consumer must receive three frames");
        expect(receivedFrameCounts[0] == 1, "FIFO order mismatch for frame 1");
        expect(receivedFrameCounts[1] == 2, "FIFO order mismatch for frame 2");
        expect(receivedFrameCounts[2] == 3, "FIFO order mismatch for frame 3");
        expect(pipeline.pendingSize() == 0, "drain must consume all pending frames");
    }

    void testEmptyDrain()
    {
        elevsafe::BackendPipeline pipeline(2);
        std::size_t callbackCount = 0;

        const auto drained = pipeline.drain([&](const elevsafe::RadarFrame&)
        {
            ++callbackCount;
        });

        expect(drained == 0, "empty drain must return zero");
        expect(callbackCount == 0, "empty drain must not call the consumer");
    }

    void testEmptyConsumerPreservesFrames()
    {
        elevsafe::BackendPipeline pipeline(2);
        pipeline.onFrame(makeFrame(9));

        elevsafe::BackendPipeline::FrameConsumer emptyConsumer;
        const auto drained = pipeline.drain(emptyConsumer);

        expect(drained == 0, "empty consumer drain must return zero");
        expect(pipeline.pendingSize() == 1, "empty consumer drain must preserve pending frames");

        std::uint32_t receivedFrameCount = 0;
        const auto consumed = pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            receivedFrameCount = frame.frameCount;
        });

        expect(consumed == 1, "preserved frame must remain drainable");
        expect(receivedFrameCount == 9, "preserved frame count mismatch");
        expect(pipeline.pendingSize() == 0, "preserved frame must be consumed by the next drain");
    }

    void testOverflowUsesFrameBufferPolicy()
    {
        elevsafe::BackendPipeline pipeline(3);
        pipeline.onFrame(makeFrame(1));
        pipeline.onFrame(makeFrame(2));
        pipeline.onFrame(makeFrame(3));
        pipeline.onFrame(makeFrame(4));

        expect(pipeline.pendingSize() == 3, "pipeline must preserve the frame capacity bound");

        std::vector<std::uint32_t> receivedFrameCounts;
        const auto drained = pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            receivedFrameCounts.push_back(frame.frameCount);
        });

        expect(drained == 3, "overflow drain must process three frames");
        expect(receivedFrameCounts.size() == 3, "overflow consumer must receive three frames");
        expect(receivedFrameCounts[0] == 2, "oldest frame was not dropped");
        expect(receivedFrameCounts[1] == 3, "unexpected middle overflow frame");
        expect(receivedFrameCounts[2] == 4, "newest overflow frame was not preserved");
    }

    void testMultipleDrains()
    {
        elevsafe::BackendPipeline pipeline(3);
        pipeline.onFrame(makeFrame(1));
        pipeline.onFrame(makeFrame(2));

        std::vector<std::uint32_t> firstDrain;
        expect(pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            firstDrain.push_back(frame.frameCount);
        }) == 2, "first drain must process two frames");
        expect(firstDrain.size() == 2, "first drain callback count mismatch");
        expect(firstDrain[0] == 1 && firstDrain[1] == 2, "first drain FIFO mismatch");
        expect(pipeline.pendingSize() == 0, "first drain must empty pending frames");

        pipeline.onFrame(makeFrame(3));
        std::vector<std::uint32_t> secondDrain;
        expect(pipeline.drain([&](const elevsafe::RadarFrame& frame)
        {
            secondDrain.push_back(frame.frameCount);
        }) == 1, "second drain must process one frame");
        expect(secondDrain.size() == 1, "second drain callback count mismatch");
        expect(secondDrain[0] == 3, "second drain received an unexpected frame");
        expect(pipeline.pendingSize() == 0, "second drain must empty pending frames");
    }

    void testZeroCapacity()
    {
        bool threw = false;
        try
        {
            elevsafe::BackendPipeline pipeline(0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }

        expect(threw, "zero capacity must throw invalid_argument");
    }
}

int main()
{
    try
    {
        testConstruction();
        testOnFrame();
        testDrainSingleFrame();
        testFrameCountGapPreservesTimestamps();
        testDrainReturnsCountAndPreservesFifo();
        testEmptyDrain();
        testEmptyConsumerPreservesFrames();
        testOverflowUsesFrameBufferPolicy();
        testMultipleDrains();
        testZeroCapacity();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "backend_pipeline_test failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "backend_pipeline_test passed\n";
    return 0;
}
