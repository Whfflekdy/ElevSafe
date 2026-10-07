#include "preprocessing/background_subtractor.h"

#include <unordered_map>

BackgroundConfig BackgroundConfig::fromConfig(const Config& cfg) {
    BackgroundConfig c;
    c.occupancy_threshold = cfg.getFloat("background.occupancy_threshold", c.occupancy_threshold);
    c.dilation = static_cast<int>(cfg.getFloat("background.dilation", static_cast<float>(c.dilation)));
    return c;
}

BackgroundSubtractor::BackgroundSubtractor(const BackgroundConfig& config) : config_(config) {}

std::int64_t BackgroundSubtractor::makeKey(int range_bin, int u_bin, int v_bin) {
    // TODO: range_bin * 1,000,000 + (u_bin + 500) * 1,000 + (v_bin + 500)
    //       곱셈이 int 범위를 넘지 않도록 static_cast<std::int64_t>(range_bin) 부터 시작
    return static_cast<std::int64_t>(range_bin)*1000000 + (u_bin+500)*1000+(v_bin+500);
}

void BackgroundSubtractor::build(const std::vector<FrameData>& empty_frames) {
    background_.clear();
    if (empty_frames.empty()) return;

    // 1. 칸마다 "점이 찍힌 프레임 수" 세기
    std::unordered_map<std::int64_t, int> hits;
    for (const auto& fd : empty_frames) {
        std::unordered_set<std::int64_t> seen;   // 이번 프레임에서 찍힌 칸 (중복 없이)
        for (const auto& p : fd.points) {
            // p.has_bins 가 false 면 건너뛰기
            // makeKey 로 열쇠를 만들어 seen 에 넣기
            if(!p.has_bins) continue;
            seen.insert(makeKey(p.range_bin,p.u_bin,p.v_bin));
        }
        for (std::int64_t k : seen) ++hits[k];
    }

    // 2. 기준 비율 이상인 칸을 배경으로 등록 (+ 주변 칸 넓히기)
    const double min_hits = config_.occupancy_threshold * empty_frames.size();
    const int d = config_.dilation;
    for (const auto& [k, count] : hits) {
        // TODO: count 가 min_hits 보다 작으면 건너뛰기
        if(count<min_hits) continue;
        // 열쇠에서 칸 번호 세 개를 다시 꺼냄
        const int r = static_cast<int>(k / 1000000);
        const int u = static_cast<int>((k / 1000) % 1000) - 500;
        const int v = static_cast<int>(k % 1000) - 500;

        for (int dr = -d; dr <= d; ++dr)
            for (int du = -d; du <= d; ++du)
                for (int dv = -d; dv <= d; ++dv)
                    background_.insert(makeKey(r + dr, u + du, v + dv));
    }
}

Frame BackgroundSubtractor::filter(const Frame& points, BGStats* stats) const {
    Frame out;
    out.reserve(points.size());
    BGStats local;
    local.input = points.size();

    for (const auto& p : points) {
        // TODO: p.has_bins 가 true 이고 그 칸이 background_ 에 있으면
        //       removed 를 하나 늘리고 continue (out 에 넣지 않음)
        if(p.has_bins &&background_.count(makeKey(p.range_bin, p.u_bin, p.v_bin))){
            ++ local.removed;
            continue;
        }
        out.push_back(p);
    }

    if (stats) *stats = local;
    return out;
}