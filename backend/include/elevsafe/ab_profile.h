#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace elevsafe
{
    class AbProfile
    {
    public:
        using Clock = std::chrono::steady_clock;
        using Duration = std::chrono::nanoseconds;
        static constexpr std::size_t kMaximumSamples = 100000;
        struct Sample
        {
            std::uint32_t id = 0;
            std::size_t points = 0;
            Clock::time_point entry{}, exit{};
            Duration pipeline{}, adapter{}, b{}, ab{};
            bool success = false;
            bool bufferReplacement = false;
        };
        struct Distribution
        {
            std::size_t count = 0, over50ms = 0;
            double average = 0, p50 = 0, p95 = 0, p99 = 0, maximum = 0;
        };
        struct Summary
        {
            Distribution callbackInterval, callbackWork, pipeline, adapter, b, ab;
            std::size_t successfulFrames = 0, duplicateIds = 0;
            std::uint64_t successfulPoints = 0;
            double windowSeconds = 0, framesPerSecond = 0, pointsPerSecond = 0;
        };

        explicit AbProfile(std::size_t capacity = 10000, std::size_t warmup = 5,
            bool callbackIntervals = true) : m_capacity(capacity), m_warmup(warmup), m_intervals(callbackIntervals)
        {
            if (capacity == 0 || capacity > kMaximumSamples || warmup > kMaximumSamples)
                throw std::invalid_argument("A-B profile sample/warm-up limit out of range");
            m_samples.reserve(capacity); // No profile vector growth on the receive path.
        }

        void record(const Sample& sample) noexcept
        {
            ++m_observed;
            m_points += sample.points;
            if (sample.success) { ++m_successful; m_successfulPoints += sample.points; }
            if (sample.bufferReplacement) ++m_replacements;
            if (m_previousId)
            {
                const std::uint32_t delta = sample.id - *m_previousId;
                if (delta != 1)
                {
                    ++m_discontinuities;
                    if (delta == 0) ++m_adjacentDuplicates;
                    else if (delta < 0x80000000u) m_forwardGapEstimate += delta - 1;
                    else ++m_backwardsOrReset;
                }
            }
            m_previousId = sample.id;
            if (m_observed <= m_warmup) return;
            if (m_samples.size() == m_capacity) { ++m_omitted; return; }
            m_samples.push_back(sample);
        }

        // Owns only timing metadata. The caller continues to own/process every frame.
        class FrameScope
        {
            AbProfile& owner;
        public:
            Sample sample;
            FrameScope(AbProfile& profile, std::uint32_t id, std::size_t points, bool replacement) : owner(profile)
            { sample.entry = Clock::now(); sample.id = id; sample.points = points; sample.bufferReplacement = replacement; }
            ~FrameScope() { sample.exit = Clock::now(); owner.record(sample); }
            FrameScope(const FrameScope&) = delete;
            FrameScope& operator=(const FrameScope&) = delete;
        };

        static Distribution distribution(std::vector<Duration> values)
        {
            Distribution result;
            if (values.empty()) return result;
            std::sort(values.begin(), values.end());
            result.count = values.size();
            long double total = 0;
            for (const auto value : values)
            {
                total += static_cast<long double>(value.count());
                if (value > std::chrono::milliseconds(50)) ++result.over50ms;
            }
            const auto milliseconds = [](Duration value) { return std::chrono::duration<double, std::milli>(value).count(); };
            const auto percentile = [&](std::size_t p) { return milliseconds(values[(values.size() * p + 99) / 100 - 1]); };
            result.average = static_cast<double>(total / values.size() / 1000000.0L);
            result.p50 = percentile(50); result.p95 = percentile(95); result.p99 = percentile(99);
            result.maximum = milliseconds(values.back());
            return result;
        }

        Summary summary() const
        {
            Summary result;
            std::vector<Duration> intervals, work, pipeline, adapter, b, ab;
            std::vector<std::uint32_t> ids;
            for (std::size_t i = 0; i < m_samples.size(); ++i)
            {
                const auto& s = m_samples[i];
                ids.push_back(s.id);
                if (m_intervals && i) intervals.push_back(std::chrono::duration_cast<Duration>(s.entry - m_samples[i - 1].entry));
                if (!s.success) continue; // Partial/failed stage durations never masquerade as successful B work.
                ++result.successfulFrames; result.successfulPoints += s.points;
                work.push_back(std::chrono::duration_cast<Duration>(s.exit - s.entry));
                pipeline.push_back(s.pipeline); adapter.push_back(s.adapter); b.push_back(s.b); ab.push_back(s.ab);
            }
            std::sort(ids.begin(), ids.end());
            for (std::size_t i = 1; i < ids.size(); ++i) if (ids[i] == ids[i - 1]) ++result.duplicateIds;
            if (!m_samples.empty())
            {
                result.windowSeconds = std::chrono::duration<double>(m_samples.back().exit - m_samples.front().entry).count();
                if (result.windowSeconds > 0)
                {
                    result.framesPerSecond = result.successfulFrames / result.windowSeconds;
                    result.pointsPerSecond = result.successfulPoints / result.windowSeconds;
                }
            }
            result.callbackInterval = distribution(std::move(intervals)); result.callbackWork = distribution(std::move(work));
            result.pipeline = distribution(std::move(pipeline)); result.adapter = distribution(std::move(adapter));
            result.b = distribution(std::move(b)); result.ab = distribution(std::move(ab));
            return result;
        }

        void writeJson(std::ostream& out) const
        {
            const auto s = summary(); // Allocations and sorting happen only after the run.
            const auto oldFlags = out.flags(); const auto oldPrecision = out.precision();
            out << std::fixed << std::setprecision(6)
                << "{\"observed_frames\":" << m_observed << ",\"observed_points\":" << m_points
                << ",\"successful_frames\":" << m_successful << ",\"successful_points\":" << m_successfulPoints
                << ",\"failed_frames\":" << m_observed - m_successful
                << ",\"warmup_excluded\":" << std::min(m_observed, m_warmup)
                << ",\"sample_limit\":" << m_capacity << ",\"stored_samples\":" << m_samples.size()
                << ",\"omitted_after_limit\":" << m_omitted
                << ",\"all_run_buffer_replacements\":" << m_replacements
                << ",\"all_run_id_discontinuities\":" << m_discontinuities
                << ",\"all_run_adjacent_duplicate_ids\":" << m_adjacentDuplicates
                << ",\"all_run_forward_gap_estimate\":" << m_forwardGapEstimate
                << ",\"all_run_backwards_or_reset\":" << m_backwardsOrReset
                << ",\"window_duplicate_ids\":" << s.duplicateIds
                << ",\"callback_intervals_available\":" << (m_intervals ? "true" : "false")
                << ",\"window_successful_frames\":" << s.successfulFrames
                << ",\"window_successful_points\":" << s.successfulPoints
                << ",\"window_seconds\":" << s.windowSeconds
                << ",\"window_frames_per_second\":" << s.framesPerSecond
                << ",\"window_points_per_second\":" << s.pointsPerSecond
                << ",\"percentile_method\":\"nearest-rank ceil(p*N)-1\",\"durations_successful_frames_only\":true";
            const auto metric = [&](const char* name, const Distribution& d)
            {
                out << ",\"" << name << "\":{\"samples\":" << d.count << ",\"average\":" << d.average
                    << ",\"p50\":" << d.p50 << ",\"p95\":" << d.p95 << ",\"p99\":" << d.p99
                    << ",\"maximum\":" << d.maximum << ",\"over_50ms\":" << d.over50ms
                    << ",\"over_50ms_percent\":" << (d.count ? 100.0 * d.over50ms / d.count : 0.0) << '}';
            };
            metric("callback_interval_ms", s.callbackInterval); metric("callback_work_ms", s.callbackWork);
            metric("pipeline_ms", s.pipeline); metric("adapter_ms", s.adapter); metric("b_ms", s.b); metric("ab_span_ms", s.ab);
            out << '}'; out.flags(oldFlags); out.precision(oldPrecision);
        }
    private:
        std::size_t m_capacity, m_warmup;
        bool m_intervals;
        std::vector<Sample> m_samples;
        std::optional<std::uint32_t> m_previousId;
        std::size_t m_observed = 0, m_successful = 0, m_replacements = 0, m_discontinuities = 0;
        std::size_t m_adjacentDuplicates = 0, m_backwardsOrReset = 0, m_omitted = 0;
        std::uint64_t m_points = 0, m_successfulPoints = 0, m_forwardGapEstimate = 0;
    };
}
