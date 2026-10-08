// dbscan.h
// [Step 4] DBSCAN 클러스터링 (비교 기준 알고리즘)
#pragma once

#include <vector>
#include "clustering/clusterer.h"
#include "common/config.h"

struct DBSCANConfig {
    float epsilon = 0.3f;   // 이웃 거리 [m] // 격자 간격이 7.16cm라서, epsilon이 그보다 충분히 커야 같은 사람 점끼리 이어짐. 
    int min_pts = 10;        // 핵심 점이 되기 위한 최소 이웃 수 (자기 자신 포함)
    float z_scale = 0.2f;   // 높이 차이에 곱할 가중치 (1보다 작으면 위아래로 긴 사람을 잘 묶음)

    static DBSCANConfig fromConfig(const Config& cfg);
};

class DBSCAN : public Clusterer {
public:
    explicit DBSCAN(const DBSCANConfig& config);

    ClusteringResult cluster(const Frame& points) const override;
    const char* name() const override { return "DBSCAN"; }

private:
    // i 번째 점의 이웃(자기 자신 포함) 위치 목록
    std::vector<int> regionQuery(const Frame& points, int i) const;

    DBSCANConfig config_;
};