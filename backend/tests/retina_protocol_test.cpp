#include "elevsafe/retina_protocol.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    using elevsafe::RadarFrame;
    using elevsafe::RetinaStreamParser;

    void appendU32LE(std::vector<std::uint8_t>& bytes, std::uint32_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    }

    void appendU64LE(std::vector<std::uint8_t>& bytes, std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }

    void appendFloatLE(std::vector<std::uint8_t>& bytes, float value)
    {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32LE(bytes, bits);
    }

    std::vector<std::uint8_t> makePacket(
        std::uint32_t frameCount,
        const std::vector<elevsafe::RadarPoint>& points,
        std::uint64_t frameMagic = elevsafe::kFrameMagic,
        std::uint32_t packageSizeOverride = 0,
        bool usePackageSizeOverride = false,
        const std::vector<std::uint8_t>& trailingPayload = {})
    {
        std::vector<std::uint8_t> payload;
        appendU64LE(payload, frameMagic);
        appendU32LE(payload, frameCount);
        appendU32LE(payload, static_cast<std::uint32_t>(points.size()));

        for (const auto& point : points)
        {
            appendFloatLE(payload, point.x);
            appendFloatLE(payload, point.y);
            appendFloatLE(payload, point.z);
            appendFloatLE(payload, point.doppler);
            appendFloatLE(payload, point.power);
        }

        for (const auto& point : points)
        {
            const auto targetId = static_cast<std::uint32_t>(point.targetId);
            appendU32LE(payload, targetId);
        }

        payload.insert(payload.end(), trailingPayload.begin(), trailingPayload.end());

        std::vector<std::uint8_t> packet;
        appendU32LE(packet, 0);
        appendU32LE(packet, elevsafe::kPacketMagic);
        appendU32LE(packet, 0);
        appendU32LE(packet, 0);
        appendU32LE(packet, usePackageSizeOverride ? packageSizeOverride : static_cast<std::uint32_t>(payload.size()));
        appendU32LE(packet, 0);
        appendU32LE(packet, 0);
        appendU32LE(packet, 0);
        appendU32LE(packet, 0);
        packet.insert(packet.end(), payload.begin(), payload.end());
        return packet;
    }

    std::vector<elevsafe::RadarPoint> samplePoints()
    {
        return {
            { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 7 },
            { -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -1 }
        };
    }

    void feed(RetinaStreamParser& parser, const std::vector<std::uint8_t>& bytes)
    {
        parser.feed(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    }

    void expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void expectSampleFrame(const std::vector<RadarFrame>& frames, std::uint32_t frameCount)
    {
        expect(frames.size() == 1, "expected one parsed frame");
        expect(frames[0].frameCount == frameCount, "unexpected frame count");
        expect(frames[0].timestampUs == 0, "parser must leave the frame timestamp at its default");
        expect(frames[0].points.size() == 2, "unexpected point count");
        expect(frames[0].points[0].targetId == 7, "unexpected first target id");
        expect(frames[0].points[1].targetId == -1, "unexpected second target id");
    }

    void testCompletePacket()
    {
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);
        feed(parser, makePacket(11, samplePoints()));
        expectSampleFrame(frames, 11);
        expect(parser.processingSummary().totalValidFrames == 1, "complete packet must create one timing sample");
    }

    void testMutableFrameCallback()
    {
        RadarFrame receivedFrame;
        bool callbackCalled = false;
        RetinaStreamParser parser([&](RadarFrame& frame)
        {
            callbackCalled = true;
            expect(frame.timestampUs == 0, "parsed frame timestamp must initially be zero");
            frame.timestampUs = 40605;
            receivedFrame = frame;
        });

        feed(parser, makePacket(25, samplePoints()));

        expect(callbackCalled, "mutable frame callback was not called");
        expect(receivedFrame.frameCount == 25, "mutable callback changed the frame count");
        expect(receivedFrame.timestampUs == 40605, "mutable callback could not assign the timestamp");
        expect(receivedFrame.points.size() == 2, "mutable callback changed the parsed points");
        expect(parser.processingSummary().totalValidFrames == 0,
            "processing metrics must remain disabled by default");
    }

    void testOneByteChunks()
    {
        const auto packet = makePacket(12, samplePoints());
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);

        for (const auto byte : packet)
            parser.feed(std::span<const std::uint8_t>(&byte, 1));

        expectSampleFrame(frames, 12);
        expect(parser.processingSummary().totalValidFrames == 1, "one-byte chunks must create one timing sample");
    }

    void testMultiplePacketsInOneChunk()
    {
        const auto first = makePacket(13, samplePoints());
        const auto second = makePacket(14, { { 9.0f, 8.0f, 7.0f, 6.0f, 5.0f, 3 } });
        std::vector<std::uint8_t> combined = first;
        combined.insert(combined.end(), second.begin(), second.end());

        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);
        feed(parser, combined);

        expect(frames.size() == 2, "expected two parsed frames");
        expect(frames[0].frameCount == 13, "unexpected first frame count");
        expect(frames[1].frameCount == 14, "unexpected second frame count");
        expect(frames[1].points.size() == 1, "unexpected second point count");
        expect(parser.processingSummary().totalValidFrames == 2, "multiple packets must create one timing sample per frame");
    }

    void testGarbageBeforePacket()
    {
        const auto packet = makePacket(15, samplePoints());
        std::vector<std::uint8_t> bytes = { 0xde, 0xad, 0xbe, 0xef, 0x01 };
        bytes.insert(bytes.end(), packet.begin(), packet.end());

        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        feed(parser, bytes);
        expectSampleFrame(frames, 15);
    }

    void testTruncatedPacket()
    {
        const auto packet = makePacket(16, samplePoints());
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);

        feed(parser, std::vector<std::uint8_t>(packet.begin(), packet.end() - 1));
        expect(frames.empty(), "truncated packet must not emit a frame");
        expect(parser.processingSummary().totalValidFrames == 0, "truncated packet must not create a timing sample");
        parser.feed(std::span<const std::uint8_t>(&packet.back(), 1));
        expectSampleFrame(frames, 16);
    }

    void testInvalidMagic()
    {
        auto packet = makePacket(17, samplePoints(), elevsafe::kFrameMagic ^ 1u);
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);
        feed(parser, packet);
        expect(frames.empty(), "invalid frame magic must be rejected");
        expect(parser.statistics().packetsRejected == 1, "invalid magic must increment rejection count");
        expect(parser.processingSummary().totalValidFrames == 0, "invalid frame magic must not create a timing sample");
    }

    void testPointCountLimit()
    {
        const auto tooManyPoints = std::vector<elevsafe::RadarPoint>(elevsafe::kMaxPointCount + 1);
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        feed(parser, makePacket(20, tooManyPoints));

        expect(frames.empty(), "point count above the limit must be rejected");
        expect(parser.statistics().packetsRejected == 1, "point count rejection must be recorded");
    }

    void testShortPackageSizes()
    {
        for (const std::uint32_t packageSize : { 0u, 8u })
        {
            std::vector<RadarFrame> frames;
            RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
            parser.setProcessingMetricsEnabled(true);
            feed(parser, makePacket(21, {}, elevsafe::kFrameMagic, packageSize, true));

            expect(frames.empty(), "package smaller than the frame header must be rejected");
            expect(parser.statistics().packetsRejected == 1, "short package rejection must be recorded");
            expect(parser.processingSummary().totalValidFrames == 0,
                "invalid package size must not create a timing sample");
        }
    }

    void testShortPointSection()
    {
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        feed(parser, makePacket(22, samplePoints(), elevsafe::kFrameMagic, static_cast<std::uint32_t>(elevsafe::kRadarFrameHeaderSize), true));

        expect(frames.empty(), "short point section must be rejected");
        expect(parser.statistics().packetsRejected == 1, "short point section rejection must be recorded");
    }

    void testTrailingPayloadIsIgnored()
    {
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        feed(parser, makePacket(23, samplePoints(), elevsafe::kFrameMagic, 0, false, { 0xaa, 0xbb, 0xcc, 0xdd }));

        expectSampleFrame(frames, 23);
        expect(parser.statistics().packetsRejected == 0, "trailing payload must not reject the point frame");
    }

    void testMaximumPointCount()
    {
        auto maximumPoints = std::vector<elevsafe::RadarPoint>(elevsafe::kMaxPointCount);
        maximumPoints.front().x = 1.0f;
        maximumPoints.back().targetId = 123;

        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        feed(parser, makePacket(24, maximumPoints));

        expect(frames.size() == 1, "maximum point count must be accepted");
        expect(frames.front().points.size() == elevsafe::kMaxPointCount, "maximum point count changed during parsing");
        expect(frames.front().points.back().targetId == 123, "maximum point payload was not fully parsed");
    }

    void testAbnormalPackageSize()
    {
        auto invalid = makePacket(18, {}, elevsafe::kFrameMagic, static_cast<std::uint32_t>(elevsafe::kMaxPacketPayloadSize + 1), true);
        const auto valid = makePacket(19, samplePoints());
        invalid.insert(invalid.end(), valid.begin(), valid.end());

        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);
        feed(parser, invalid);

        expectSampleFrame(frames, 19);
        expect(parser.statistics().packetsRejected >= 1, "abnormal package size must be rejected");
        expect(parser.processingSummary().totalValidFrames == 1,
            "only the valid packet after an abnormal package size must create a timing sample");
    }

    void testProcessingSummary()
    {
        elevsafe::ParserProcessingMetrics metrics;
        for (int milliseconds = 1; milliseconds <= 25; ++milliseconds)
            metrics.record(std::chrono::milliseconds(milliseconds));

        const auto summary = metrics.summary();
        expect(summary.totalValidFrames == 25, "summary total valid frame count mismatch");
        expect(summary.warmupExcluded == 5, "summary warm-up count mismatch");
        expect(summary.measuredFrames == 20, "summary measured frame count mismatch");
        expect(summary.averageProcessingMs == 15.5, "summary average mismatch");
        expect(summary.p95ProcessingMs == 24.0, "summary p95 mismatch");
        expect(summary.maximumProcessingMs == 25.0, "summary maximum mismatch");
    }

    void testProcessingMetricsFreezeTransition()
    {
        elevsafe::ParserProcessingMetrics metrics;
        metrics.setFrameLimit(2);

        expect(!metrics.record(std::chrono::milliseconds(1)), "first limited sample must not freeze metrics");
        expect(metrics.record(std::chrono::milliseconds(2)), "limit-reaching sample must freeze metrics once");
        expect(!metrics.record(std::chrono::milliseconds(3)), "frozen metrics must not freeze a second time");
        expect(metrics.frozen(), "metrics must remain frozen after reaching the limit");
        expect(metrics.summary().totalValidFrames == 2, "frozen metrics must preserve the configured frame limit");
    }

    void testProcessingFrameLimitFreezesAfterValidFrames()
    {
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);
        parser.setProcessingMetricsFrameLimit(164);

        std::vector<std::uint8_t> packets;
        for (std::uint32_t frameCount = 1; frameCount <= 165; ++frameCount)
        {
            const auto packet = makePacket(frameCount, samplePoints());
            packets.insert(packets.end(), packet.begin(), packet.end());
        }
        feed(parser, packets);

        const auto frozenSummary = parser.processingSummary();
        expect(frames.size() == 165, "frame delivery must continue after metrics freeze");
        expect(parser.processingMetricsFrozen(), "metrics must freeze at the configured valid frame limit");
        expect(frozenSummary.totalValidFrames == 164, "frame limit must cap valid timing samples");
        expect(frozenSummary.warmupExcluded == 5, "frame limit summary warm-up count mismatch");
        expect(frozenSummary.measuredFrames == 159, "frame limit summary measured count mismatch");

        feed(parser, makePacket(166, samplePoints()));
        const auto summaryAfterAdditionalFrame = parser.processingSummary();
        expect(frames.size() == 166, "parser must continue to deliver frames after metrics freeze");
        expect(summaryAfterAdditionalFrame.totalValidFrames == frozenSummary.totalValidFrames,
            "additional frames must not change frozen timing sample count");
        expect(summaryAfterAdditionalFrame.measuredFrames == frozenSummary.measuredFrames,
            "additional frames must not change frozen measured count");
        expect(summaryAfterAdditionalFrame.averageProcessingMs == frozenSummary.averageProcessingMs,
            "additional frames must not change frozen average");
        expect(summaryAfterAdditionalFrame.p95ProcessingMs == frozenSummary.p95ProcessingMs,
            "additional frames must not change frozen p95");
        expect(summaryAfterAdditionalFrame.maximumProcessingMs == frozenSummary.maximumProcessingMs,
            "additional frames must not change frozen maximum");
    }

    void testProcessingFrameLimitCountsOnlyValidFrames()
    {
        std::vector<RadarFrame> frames;
        RetinaStreamParser parser([&](const RadarFrame& frame) { frames.push_back(frame); });
        parser.setProcessingMetricsEnabled(true);
        parser.setProcessingMetricsFrameLimit(1);

        feed(parser, makePacket(30, samplePoints(), elevsafe::kFrameMagic ^ 1u));
        expect(!parser.processingMetricsFrozen(), "invalid frame must not consume the timing frame limit");
        expect(parser.processingSummary().totalValidFrames == 0, "invalid frame must not create a timing sample");

        const auto validPacket = makePacket(31, samplePoints());
        feed(parser, std::vector<std::uint8_t>(validPacket.begin(), validPacket.end() - 1));
        expect(!parser.processingMetricsFrozen(), "truncated packet must not consume the timing frame limit");
        expect(parser.processingSummary().totalValidFrames == 0, "truncated packet must not create a timing sample");

        parser.feed(std::span<const std::uint8_t>(&validPacket.back(), 1));
        const auto summary = parser.processingSummary();
        expect(parser.processingMetricsFrozen(), "completed valid frame must consume the timing frame limit");
        expect(summary.totalValidFrames == 1, "valid frame must create one timing sample");
        expect(summary.warmupExcluded == 1, "short frame limit must exclude its valid frame as warm-up");
        expect(summary.measuredFrames == 0, "short frame limit must safely allow no measured frames");
        expect(summary.averageProcessingMs == 0.0, "short frame limit average must remain zero");
        expect(summary.p95ProcessingMs == 0.0, "short frame limit p95 must remain zero");
        expect(summary.maximumProcessingMs == 0.0, "short frame limit maximum must remain zero");
    }
}

int main()
{
    try
    {
        testCompletePacket();
        testMutableFrameCallback();
        testOneByteChunks();
        testMultiplePacketsInOneChunk();
        testGarbageBeforePacket();
        testTruncatedPacket();
        testInvalidMagic();
        testPointCountLimit();
        testShortPackageSizes();
        testShortPointSection();
        testTrailingPayloadIsIgnored();
        testMaximumPointCount();
        testAbnormalPackageSize();
        testProcessingSummary();
        testProcessingMetricsFreezeTransition();
        testProcessingFrameLimitFreezesAfterValidFrames();
        testProcessingFrameLimitCountsOnlyValidFrames();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "retina_protocol_test failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "retina_protocol_test passed\n";
    return 0;
}
