#include "elevsafe/ab_profile.h"
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace
{
    void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    elevsafe::AbProfile::Sample sample(std::uint32_t id, int ms)
    {
        elevsafe::AbProfile::Sample s;
        s.id = id; s.points = 10;
        s.entry = elevsafe::AbProfile::Clock::time_point{} + std::chrono::milliseconds(id * 200ull);
        s.exit = s.entry + std::chrono::milliseconds(ms);
        s.pipeline = s.adapter = s.b = s.ab = std::chrono::milliseconds(ms); s.success = true;
        return s;
    }
}
int main()
{
    try
    {
        using P = elevsafe::AbProfile;
        expect(P{}.summary().b.count == 0, "empty profile must have no samples");
        std::ostringstream empty; P{}.writeJson(empty);
        expect(empty.str().find("\"window_seconds\":0.000000") != std::string::npos, "empty report invalid");
        P profile(100, 5);
        for (std::uint32_t i = 1; i <= 105; ++i) profile.record(sample(i, static_cast<int>(i)));
        const auto s = profile.summary();
        expect(s.b.count == 100 && s.b.average == 55.5 && s.b.p50 == 55 && s.b.p95 == 100
            && s.b.p99 == 104 && s.b.maximum == 105, "warm-up or nearest-rank percentiles changed");
        expect(s.b.over50ms == 55 && s.callbackInterval.count == 99 && s.callbackInterval.p50 == 200,
            "threshold/interval calculation changed");
        P capped(2, 1);
        capped.record(sample(10, 1)); capped.record(sample(11, 50)); capped.record(sample(11, 51));
        auto failure = sample(14, 20); failure.success = false; failure.bufferReplacement = true; capped.record(failure);
        std::ostringstream report; capped.writeJson(report);
        expect(capped.summary().b.count == 2 && capped.summary().b.over50ms == 1, "sample limit or strict 50ms boundary");
        for (const auto* token : {"\"observed_frames\":4", "\"failed_frames\":1", "\"omitted_after_limit\":1",
            "\"all_run_buffer_replacements\":1", "\"all_run_adjacent_duplicate_ids\":1", "\"window_duplicate_ids\":1",
            "\"all_run_forward_gap_estimate\":2"})
            expect(report.str().find(token) != std::string::npos, "bounded/full-run counter mismatch");
        P failed(2, 0); failed.record(failure);
        expect(failed.summary().b.count == 0, "failed work included in successful B durations");
        P wrapping(2, 0); wrapping.record(sample(std::numeric_limits<std::uint32_t>::max(), 0)); wrapping.record(sample(0, 0));
        std::ostringstream wrapped; wrapping.writeJson(wrapped);
        expect(wrapped.str().find("\"all_run_id_discontinuities\":0") != std::string::npos, "uint32 wrap mistaken for loss");
        P offline(2, 0, false); offline.record(sample(1, 0)); offline.record(sample(2, 0));
        expect(offline.summary().callbackInterval.count == 0, "offline callback cadence must be unavailable");
        for (const std::size_t capacity : {std::size_t{0}, P::kMaximumSamples + 1})
        {
            bool rejected = false;
            try { P invalid(capacity); } catch (const std::invalid_argument&) { rejected = true; }
            expect(rejected, "invalid sample capacity accepted");
        }
        P maximum(P::kMaximumSamples, P::kMaximumSamples);
        maximum.record(sample(1, 0)); expect(maximum.summary().b.count == 0, "warm-up boundary failed");
    }
    catch (const std::exception& e) { std::cerr << "ab_profile_test failed: " << e.what() << '\n'; return 1; }
    std::cout << "ab_profile_test passed\n";
}
