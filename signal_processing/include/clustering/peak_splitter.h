// peak_splitter.h
// [Step 5] DBSCAN 이 합친 사람을 머리 높이 봉우리로 나눈다.
#pragma once

#include <vector>
#include "clustering/clusterer.h"
#include "common/config.h"

namespace elevsafe::sp {

struct PeakSplitConfig {
    bool enabled = false;       // 꺼 두면 DBSCAN 결과 그대로
    float cell = 0.10f;         // 높이 지도 칸 크기 [m]
    float min_height = 0.9f;    // 봉우리 최소 높이 [m]
    float min_valley = 0.15f;   // 두 봉우리 사이가 낮은 쪽보다 이만큼 낮아야 함 [m]
    float min_distance = 0.30f; // 봉우리 사이 최소 수평 거리 [m]
    int min_points = 15;        // 나눈 뒤 조각의 최소 점 수
    float max_width = 0.70f;    // 봉우리 1개인데 이보다 넓으면 합쳐짐 의심 [m]

    static PeakSplitConfig fromConfig(const Config& cfg);
};

struct SplitResult {
    std::vector<int> labels;          // 나눈 뒤 점별 덩어리 번호
    int num_clusters = 0;
    std::vector<char> maybe_merged;   // 덩어리별 합쳐짐 표시 (1 = 의심)
    int split_count = 0;              // 이번 프레임에 나눈 덩어리 수
};

class PeakSplitter {
public:
    explicit PeakSplitter(const PeakSplitConfig& config);
    SplitResult split(const Frame& points, const ClusteringResult& cr) const;
private:
    PeakSplitConfig config_;
};

}  // namespace elevsafe::sp