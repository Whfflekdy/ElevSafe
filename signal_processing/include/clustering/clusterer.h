// clusterer.h
// [Step 4] 클러스터링 알고리즘의 공통 틀
// DBSCAN 과 이후 새로 만들 알고리즘이 모두 이 틀을 따르면,
// 파이프라인에서는 알고리즘을 바꿔 끼우기만 하면 된다.
#pragma once

#include <vector>
#include "common/radar_point.h"

struct ClusteringResult {
    std::vector<int> labels;   // 점마다 덩어리 번호 (입력 순서와 같음, -1 = 노이즈)
    int num_clusters = 0;      // 덩어리 개수 (번호는 0 ~ num_clusters-1)
};

class Clusterer {
public:
    virtual ~Clusterer() = default;
    virtual ClusteringResult cluster(const Frame& points) const = 0;
    virtual const char* name() const = 0;
};