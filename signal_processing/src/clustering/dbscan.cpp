#include "clustering/dbscan.h"

#include <deque>

namespace {
constexpr int UNVISITED = -2;   // 아직 안 본 점
constexpr int NOISE = -1;       // 노이즈
}

DBSCANConfig DBSCANConfig::fromConfig(const Config& cfg) {
    DBSCANConfig c;
    c.epsilon = cfg.getFloat("dbscan.epsilon", c.epsilon);
    c.min_pts = static_cast<int>(cfg.getFloat("dbscan.min_pts", static_cast<float>(c.min_pts)));
    c.z_scale = cfg.getFloat("dbscan.z_scale", c.z_scale);
    return c;
}

DBSCAN::DBSCAN(const DBSCANConfig& config) : config_(config) {}

std::vector<int> DBSCAN::regionQuery(const Frame& points, int i) const {
    std::vector<int> neighbors;
    const RadarPoint& a = points[i];
    const float eps2 = config_.epsilon * config_.epsilon;   // 제곱끼리 비교하면 sqrt 가 필요 없다

    for (int j = 0; j < static_cast<int>(points.size()); ++j) {
        const RadarPoint& b = points[j];
        // dx, dy 는 그대로, dz 는 z_scale 을 곱한 높이 차이로 계산
        const float dx = a.x-b.x;
        const float dy = a.y-b.y;
        // 높이 차이를 줄여서 계산하면 위아래로 긴 사람의 머리와 발 쪽 점이 더 쉽게 이웃으로 묶이기 때문에 
        const float dz = (a.z-b.z)*config_.z_scale;

        // dx*dx + dy*dy + dz*dz 가 eps2 이하이면 neighbors 에 j 추가
        if(dx*dx+dy*dy+dz*dz <=eps2) neighbors.push_back(j);
    }
    return neighbors;
}

ClusteringResult DBSCAN::cluster(const Frame& points) const {
    const int n = static_cast<int>(points.size());
    ClusteringResult result;
    result.labels.assign(n, UNVISITED);
    int cid = 0;   // 다음에 만들 덩어리 번호

    for (int i = 0; i < n; ++i) {
        if (result.labels[i] != UNVISITED) continue;

        std::vector<int> neighbors = regionQuery(points, i);
        if (static_cast<int>(neighbors.size()) < config_.min_pts) {
            result.labels[i] = NOISE;   // 일단 노이즈 (나중에 덩어리 가장자리로 바뀔 수 있음)
            continue;
        }

        // 새 덩어리 시작: i 의 이웃들을 줄 세워 하나씩 퍼뜨린다
        result.labels[i] = cid;
        std::deque<int> queue(neighbors.begin(), neighbors.end());

        while (!queue.empty()) {
            const int j = queue.front();
            queue.pop_front();

            // labels[j] 가 NOISE 면 cid 로 바꾸고 continue
            //         (핵심 점 옆의 외톨이 = 덩어리 가장자리)
            if(result.labels[j]==NOISE){
                result.labels[j] = cid; continue;
            }
            // labels[j] 가 UNVISITED 가 아니면 이미 처리된 점이니 continue
            if(result.labels[j]!=UNVISITED) continue;
            
            result.labels[j] = cid;
            // j 의 이웃을 regionQuery 로 구해서, 그 개수가 min_pts 이상이면
            // (j 도 핵심 점) 그 이웃들을 모두 queue 뒤에 추가
            std::vector<int> j_neighbors = regionQuery(points, j);
            if(static_cast<int>(j_neighbors.size())>=config_.min_pts) // 핵심 점이면(j포함)
                for(int k: j_neighbors)queue.push_back(k);
        }
        ++cid;
    }

    result.num_clusters = cid;
    return result;
}