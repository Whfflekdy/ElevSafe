#include "clustering/peak_splitter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace elevsafe::sp {

namespace {
struct Peak { float x, y, z; };
using Cell = std::pair<int, int>;        // 칸 번호 (x, y)
using HeightMap = std::map<Cell, float>; // 칸 → 그 칸의 가장 높은 z

float dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

// 거리만 보면 머리와 어깨도 두 봉우리가 될 수 있으므로 두 봉우리 사이가 충분히 낮은지도 확인해봐야 함 .
// 두 봉우리를 잇는 선 위에서 가장 낮은 높이
float valleyBetween(const HeightMap& hm, float cell, const Peak& a, const Peak& b) {
    const float len = std::sqrt(dist2(a.x, a.y, b.x, b.y));
    const int steps = std::max(1, static_cast<int>(len / cell));
    float low = std::min(a.z, b.z);
    for (int i = 1; i < steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        // a 와 b 사이 t 위치의 칸 번호 Cell c 계산 (std::floor(좌표 / cell))
        const Cell c{static_cast<int>(std::floor((a.x + (b.x - a.x) * t) / cell)),
                     static_cast<int>(std::floor((a.y + (b.y - a.y) * t) / cell))};
        // hm 에서 c 찾기. 없으면 continue (점이 듬성해서 생긴 구멍일 수 있음)
        const auto it = hm.find(c);
        // 있으면 low = 그 칸 높이와 low 중 작은 값
        if(it == hm.end()) continue; // 빈칸은 점이 듬성듬성해서 생긴 구멍일 수 있음(한 사람이 둘로 나뉘지 않게)
        low = std::min(low, it->second);
    }
    return low;
}

std::vector<Peak> findPeaks(const HeightMap& hm, const PeakSplitConfig& cfg) {
    std::vector<Peak> cand;
    // hm 의 칸마다 z >= cfg.min_height 이면 cand 에 추가
    // 칸 중심 x = (c.first + 0.5f) * cfg.cell, y 도 같은 방식
    for(const auto& [c, z]: hm){
        if(z<cfg.min_height) continue;
        // 칸 번호에 0.5를 더해 칸의 가운데 좌표로 바꿈
        cand.push_back({(c.first + 0.5f)* cfg.cell, 
                        (c.second + 0.5f)* cfg.cell, z});
    }

    // 가장 높은 곳부터 봐야 머리가 먼저 봉우리로 뽑힘
    std::sort(cand.begin(), cand.end(),
              [](const Peak& a, const Peak& b) { return a.z > b.z; });

    std::vector<Peak> peaks;
    // cand 의 p 를 차례로 보며, 이미 뽑은 peaks 의 q 모두와 비교
    const float d2 = cfg.min_distance * cfg.min_distance;
    
    for(const Peak& p : cand){
        bool ok = true;
        for(const Peak& q: peaks){
            //   수평 거리가 cfg.min_distance 보다 가까우면 탈락
            const bool too_close = dist2(p.x, p.y, q.x, q.y)<d2;
            //   min(p.z, q.z) - valleyBetween(...) < cfg.min_valley 이면 탈락
            const bool no_valley = 
                std::min(p.z, q.z) - valleyBetween(hm, cfg.cell, p, q)<cfg.min_valley;
            if(too_close || no_valley){
                ok = false;
                break;
            }
        }
        //   끝까지 통과하면 peaks 에 추가
        if(ok) peaks.push_back(p);
    }
    return peaks;
}
}  // namespace

PeakSplitConfig PeakSplitConfig::fromConfig(const Config& cfg) {
    PeakSplitConfig c;
    c.enabled = cfg.getFloat("peak_split.enabled", 0.0f) != 0.0f;
    c.cell = cfg.getFloat("peak_split.cell", c.cell);
    c.min_height = cfg.getFloat("peak_split.min_height", c.min_height);
    c.min_valley = cfg.getFloat("peak_split.min_valley", c.min_valley);
    c.min_distance = cfg.getFloat("peak_split.min_distance", c.min_distance);
    c.min_points = static_cast<int>(
    cfg.getFloat("peak_split.min_points", static_cast<float>(c.min_points)));
    c.max_width = cfg.getFloat("peak_split.max_width", c.max_width);
    return c;
}

PeakSplitter::PeakSplitter(const PeakSplitConfig& config) : config_(config) {}

// 덩어리별 높이 지도 만들기 
SplitResult PeakSplitter::split(const Frame& points, const ClusteringResult& cr) const {
    SplitResult out;
    out.labels = cr.labels;
    out.num_clusters = cr.num_clusters;
    out.maybe_merged.assign(cr.num_clusters, 0);
    if (!config_.enabled) return out; // 꺼져 있으면 DBSCAN 결과 그대로

    // 덩어리별 점 번호 모으기
    std::vector<std::vector<int>> members(cr.num_clusters);
    for (std::size_t i = 0; i < points.size(); ++i)
        if (cr.labels[i] >= 0) members[cr.labels[i]].push_back(static_cast<int>(i));

    for (int k = 0; k < cr.num_clusters; ++k) {
        HeightMap hm;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        // members[k] 의 점마다 칸 번호를 구해 hm 에 최대 z 기록, x0·x1·y0·y1 넓히기
        for(int i: members[k]){
            const RadarPoint &p = points[i];
            const Cell c{static_cast<int>(std::floor(p.x / config_.cell)),
                         static_cast<int>(std::floor(p.y / config_.cell))};

            auto it = hm.find(c);
            if(it == hm.end()) hm[c] = p.z;
            else it -> second = std::max(it->second, p.z);
            // 봉우리가 하나인데도 유난히 넓은 덩어리를 찾을 때 사용. 
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
        // 나눌지 결정하기
        // 봉우리가 2개 미만이면 나눌 근거가 없음.
        // 다만 폭이 넓으면 아이의 봉우리를 놓쳤을 수 있으니 나누지 않고 c에게 표시만 함. 
        const std::vector<Peak> peaks = findPeaks(hm, config_);
        const bool wide = std::max(x1 - x0, y1 - y0) > config_.max_width;
        if (peaks.size() < 2) {
            if (wide) out.maybe_merged[k] = 1;   // 나누지는 못하지만 C 에게 알림
            continue;
        }
        // 점을 가까운 봉우리에 나누기
        // 수평 거리만 봐야 다리와 몸통도 자기 머리쪽으로 감. 
        // z까지 넣게 되면 낮은 점이 어느 머리에서나 멀어져서 판단이 흐려짐. 
        // 점마다 수평으로 가장 가까운 봉우리 번호를 owner[m] 에 기록, counts 세기
        std::vector<int> owner(members[k].size()), counts(peaks.size(), 0);

        for(std::size_t m = 0; m<members[k].size(); ++m){
            const RadarPoint& p = points[members[k][m]];
            int best = 0;
            for(std::size_t q = 1; q<peaks.size(); ++q){
                if(dist2(p.x, p.y, peaks[q].x, peaks[q].y)<
                   dist2(p.x, p.y, peaks[best].x, peaks[best].y)){
                   best= static_cast<int>(q);    
                }
            }
            owner[m] = best;
            counts[best]++;
        }

        // 어떤 조각이 min_points 보다 작으면 가짜 봉우리일 수 있으므로 나누지 말고
        // out.maybe_merged[k] = 1 로 표시한 뒤 continue
        bool enough = true;
        for(int n: counts){
            if(n<config_.min_points) enough = false;
        }
        if(!enough){
            out.maybe_merged[k] = 1;
            continue;
        }

        // 새 번호 붙이기. 
        // 첫 조각은 번호 k 유지, 나머지는 새 번호
        // 번호가 비지 않고 이어지므로 summerize가 그대로 동작함. 
        // 나눈 덩어리도 maybe_merged를 1로 둠(잘못 나눴을 수 있으니 C가 추적으로 최종 판단하게 하기 위함.)
        std::vector<int> ids(peaks.size());
        ids[0] = k;
        out.maybe_merged[k] = 1;
        for (std::size_t q = 1; q < peaks.size(); ++q) {
            ids[q] = out.num_clusters++;
            out.maybe_merged.push_back(1);
        }
        // 점마다 out.labels[members[k][m]] = ids[owner[m]]
        for(std::size_t m = 0; m<members[k].size(); ++m){
            out.labels[members[k][m]] = ids[owner[m]];
        }
        out.split_count++;
    }
    return out;
}

}  // namespace elevsafe::sp