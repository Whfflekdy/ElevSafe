#include "elevsafe/backend_pipeline.h"
#include "elevsafe/ab_profile.h"
#include "elevsafe/signal_adapter.h"
#include "io/csv_io.h"
#include "pipeline/signal_pipeline.h"

#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
    void expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void testRuntimeBoundary(const elevsafe::sp::Config& config)
    {
        elevsafe::sp::SignalPipeline signal(config);
        expect(signal.ok(), "valid config must initialize B");
        expect(!signal.usesBackground(), "test must not depend on a local background map");
        elevsafe::BackendPipeline pipeline(1);
        elevsafe::RadarFrame frame;
        frame.frameCount = 7;
        frame.timestampUs = 50000;
        for (int i = 0; i < 12; ++i)
            frame.points.push_back({0.01f * i, 0.6f, 1.0f, -0.5f, 100.0f, 255});
        pipeline.onFrame(frame);
        bool called = false;
        const auto drained = pipeline.drain([&](const elevsafe::RadarFrame& pending)
        {
            const auto input = elevsafe::toSignalFrame(pending);
            const elevsafe::sp::FrameContext context;
            expect(context.door_state == elevsafe::sp::DoorState::UNKNOWN, "context must remain UNKNOWN");
            const auto result = signal.process(input, context);
            expect(result.frame_id == 7 && result.timestamp_us == 50000, "B changed metadata");
            expect(result.clusters.size() == 1, "expected one synthetic cluster");
            const auto& cluster = result.clusters.front();
            expect(cluster.num_points == 12 && cluster.zone == elevsafe::sp::Zone::CABIN, "cluster contents mismatch");
            expect(cluster.mean_doppler == -0.5f && !cluster.maybe_merged, "cluster summary mismatch");
            expect(result.estimated_door_state == elevsafe::sp::DoorState::UNKNOWN, "first door observation must be UNKNOWN");
            expect(input.points[0].zone == elevsafe::sp::Zone::NONE, "B mutated its input");
            called = true;
        });
        expect(called && drained == 1 && pipeline.pendingSize() == 0, "runtime boundary did not drain");

        frame.frameCount = 8;
        frame.timestampUs = 101234;
        frame.points.clear();
        const auto empty = signal.process(elevsafe::toSignalFrame(frame), {});
        expect(empty.frame_id == 8 && empty.timestamp_us == 101234 && empty.clusters.empty(), "empty frame lost");
        expect(empty.estimated_door_state == elevsafe::sp::DoorState::CLOSED, "empty frame must advance B state");

        pipeline.onFrame({std::numeric_limits<std::uint32_t>::max(), 0, {}});
        std::size_t rejected = 0;
        pipeline.drain([&](const elevsafe::RadarFrame& pending)
        {
            try { elevsafe::toSignalFrame(pending); }
            catch (const std::overflow_error&) { ++rejected; }
        });
        pipeline.onFrame(frame);
        const auto resumed = pipeline.drain([&](const elevsafe::RadarFrame& pending)
        {
            expect(signal.process(elevsafe::toSignalFrame(pending), {}).clusters.empty(), "next frame failed");
        });
        expect(rejected == 1 && resumed == 1, "frame rejection prevented later processing");
    }

    void testOfflineEquivalence(const elevsafe::sp::Config& config, const char* csv,
        elevsafe::AbProfile* profile = nullptr, std::optional<bool>* background = nullptr)
    {
        const auto frames = elevsafe::sp::readFramesCSV(csv);
        expect(!frames.empty(), "B sample CSV must yield frames");
        elevsafe::sp::SignalPipeline offline(config);
        elevsafe::sp::SignalPipeline live(config);
        expect(offline.ok() && live.ok(), "offline profile requires valid B initialization");
        expect(offline.usesBackground() == live.usesBackground(), "direct/replay background modes differ");
        if (background) *background = live.usesBackground();
        elevsafe::BackendPipeline backend(1);
        std::size_t clusters = 0;
        for (const auto& source : frames)
        {
            elevsafe::RadarFrame frame;
            expect(std::in_range<std::uint32_t>(source.frame_id) && std::in_range<std::uint64_t>(source.timestamp_us),
                "CSV metadata cannot be represented by RadarFrame");
            frame.frameCount = static_cast<std::uint32_t>(source.frame_id);
            frame.timestampUs = static_cast<std::uint64_t>(source.timestamp_us);
            for (const auto& point : source.points)
                frame.points.push_back({point.x, point.y, point.z, point.doppler, point.power, point.target_id});
            const auto expected = offline.process(source, {});
            elevsafe::sp::PipelineResult actual;
            if (profile)
            {
                using P = elevsafe::AbProfile;
                P::FrameScope measured(*profile, frame.frameCount, frame.points.size(), backend.pendingSize() >= 1);
                const auto start = P::Clock::now();
                backend.onFrame(frame);
                backend.drain([&](const auto& pending)
                {
                    const auto adapterStart = P::Clock::now();
                    const auto input = elevsafe::toSignalFrame(pending);
                    measured.sample.adapter = P::Clock::now() - adapterStart;
                    const auto bStart = P::Clock::now();
                    actual = live.process(input, {});
                    const auto bEnd = P::Clock::now();
                    measured.sample.b = bEnd - bStart;
                    measured.sample.ab = bEnd - adapterStart;
                    measured.sample.success = true;
                });
                measured.sample.pipeline = P::Clock::now() - start;
            }
            else actual = live.process(elevsafe::toSignalFrame(frame), {});
            expect(actual.frame_id == expected.frame_id && actual.timestamp_us == expected.timestamp_us, "offline metadata mismatch");
            expect(actual.estimated_door_state == expected.estimated_door_state, "offline door state mismatch");
            expect(actual.clusters.size() == expected.clusters.size(), "offline cluster count mismatch");
            clusters += actual.clusters.size();
            for (std::size_t i = 0; i < actual.clusters.size(); ++i)
            {
                const auto& a = actual.clusters[i];
                const auto& b = expected.clusters[i];
                expect(a.cluster_id == b.cluster_id && a.num_points == b.num_points, "offline cluster identity mismatch");
                expect(a.cx == b.cx && a.cy == b.cy && a.cz == b.cz, "offline centroid mismatch");
                expect(a.x_min == b.x_min && a.x_max == b.x_max && a.y_min == b.y_min && a.y_max == b.y_max
                    && a.z_min == b.z_min && a.z_max == b.z_max, "offline bounds mismatch");
                expect(a.z_top == b.z_top && a.mean_doppler == b.mean_doppler
                    && a.zone == b.zone && a.maybe_merged == b.maybe_merged, "offline summary mismatch");
            }
        }
        std::cout << "Offline equivalence: frames=" << frames.size() << " clusters=" << clusters << '\n';
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 4 && std::string_view(argv[1]) == "--ab-profile")
        {
            elevsafe::sp::Config config;
            expect(config.load(argv[2]), "offline profile config load failed");
            elevsafe::AbProfile profile(10000, 5, false);
            std::optional<bool> background;
            bool completed = false;
            try { testOfflineEquivalence(config, argv[3], &profile, &background); completed = true; }
            catch (const std::exception& error) { std::cerr << "Offline profile incomplete: " << error.what() << '\n'; }
            std::cout << "AB_PROFILE_JSON {\"mode\":\"offline_equivalence\",\"equivalence_complete\":"
                      << (completed ? "true" : "false") << ",\"background_enabled\":"
                      << (background ? (*background ? "true" : "false") : "null")
                      << ",\"wall_window_includes_reference_and_validation\":true,\"metrics\":";
            profile.writeJson(std::cout);
            std::cout << "}\n";
            return completed ? 0 : 1;
        }
        expect(argc == 4, "expected valid config, B sample CSV and invalid config paths");
        elevsafe::sp::Config config;
        expect(config.load(argv[1]), "test config load failed");
        elevsafe::sp::Config invalid;
        expect(invalid.load(argv[3]), "invalid config fixture must be readable");
        expect(!elevsafe::sp::SignalPipeline(invalid).ok(), "invalid ROI must fail initialization");
        testRuntimeBoundary(config);
        testOfflineEquivalence(config, argv[2]);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "signal_integration_test failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "signal_integration_test passed\n";
    return 0;
}
