// signal_pipeline.cpp
#include "pipeline/signal_pipeline.h"

#include <algorithm>
#include <cmath>
#include <string>

// ---------------------------------------------------------------------------
// 생성자: 설정으로 부품을 한 번 준비
// 멤버는 헤더에 선언한 순서대로 초기화된다 (순서가 다르면 컴파일러가 경고)
// ---------------------------------------------------------------------------
SignalPipeline::SignalPipeline(const Config& cfg)
    : transform_(SensorPose::fromConfig(cfg)),
      polar_(PolarGridConfig::fromConfig(cfg)),
      roi_(ROIConfig::fromConfig(cfg)),
      bg_(BackgroundConfig::fromConfig(cfg)),
      dbscan_(DBSCANConfig::fromConfig(cfg)),
      door_est_(DoorStateConfig::fromConfig(cfg)) {
    // 설정 검사: ROI 범위와 문 상태 설정이 둘 다 말이 되어야 사용 가능
    // (추정기는 자기 설정을 밖으로 안 보여 주니, 같은 설정을 한 번 더 읽어 검사)
    ok_ = roi_.config().isValid() && DoorStateConfig::fromConfig(cfg).isValid();

    // 배경 맵: 경로가 있고 불러오기에 성공했을 때만 사용
    const std::string map_path = cfg.getString("background.map_path", "");
    use_bg_ = !map_path.empty() && bg_.load(map_path);
}

// ---------------------------------------------------------------------------
// 프레임 하나 처리
// ---------------------------------------------------------------------------
PipelineResult SignalPipeline::process(const FrameData& frame, const FrameContext& ctx,
                                       Frame* processed_points) {
    (void)ctx;  // 지금은 참고만 (나중에 백엔드 문 상태를 쓰는 규칙이 생기면 사용)

    // 복사본으로 작업: A 가 넘긴 frame 은 그대로 두기
    Frame pts = frame.points;
    polar_.assignBins(pts);    // 격자 번호 (센서 좌표 기준이라 변환 전에)
    transform_.apply(pts);     // 센서 좌표 → 엘리베이터 좌표

    // 문 상태 추정: ROI 가 문 너머 점을 지우기 전에
    const DoorState ds = door_est_.update(pts);

    // ROI
    ROIStats rs;
    Frame kept = roi_.filter(pts, &rs);
    stats_.roi += rs;

    // 정적 배경 제거
    if (use_bg_) {
        BGStats bs;
        kept = bg_.filter(kept, &bs);
        stats_.bg.input   += bs.input;
        stats_.bg.removed += bs.removed;
    }

    // Step 3: outlier_filter (SOR, RCS) 자리

    // DBSCAN: 점마다 덩어리 번호 기록
    const ClusteringResult cr = dbscan_.cluster(kept);
    for (std::size_t i = 0; i < kept.size(); ++i) {
        kept[i].cluster_id = cr.labels[i];
        if (cr.labels[i] == -1) stats_.noise++;
    }
    stats_.clusters += static_cast<std::size_t>(cr.num_clusters);
    stats_.frames++;

    // 결과 만들기
    PipelineResult r;
    r.frame_id = frame.frame_id;
    r.timestamp_us = frame.timestamp_us;
    r.clusters = summarize(kept, cr);          // kept 를 넘기기 전에 요약부터!
    r.estimated_door_state = ds;

    // 오프라인용 점 출력 (요약이 끝났으니 이제 넘겨도 됨)
    if (processed_points != nullptr) *processed_points = std::move(kept);
    return r;
}

// ---------------------------------------------------------------------------
// 덩어리 요약 (Step 5 최소판)
// ---------------------------------------------------------------------------
std::vector<Cluster> SignalPipeline::summarize(const Frame& points, const ClusteringResult& cr) {
    const int n_cl = cr.num_clusters;
    std::vector<Cluster> out(n_cl);
    if (n_cl == 0) return out;

    // 덩어리마다 모아 둘 것 (합은 double 로: float 로 수백 번 더하면 오차가 쌓임)
    std::vector<double> sx(n_cl, 0.0), sy(n_cl, 0.0), sz(n_cl, 0.0), sd(n_cl, 0.0);
    std::vector<int> n_door(n_cl, 0), n_cabin(n_cl, 0);
    std::vector<std::vector<float>> zs(n_cl);   // z_top 계산용 높이 목록

    // 1. 점을 한 번 훑으며 모으기
    for (std::size_t i = 0; i < points.size(); ++i) {
        const int k = cr.labels[i];
        if (k < 0) continue;                    // 노이즈는 건너뜀
        const RadarPoint& p = points[i];
        Cluster& c = out[k];

        // 범위: 첫 점이면 그 점으로 시작, 아니면 넓혀 가기
        if (c.num_points == 0) {
            c.x_min = c.x_max = p.x;
            c.y_min = c.y_max = p.y;
            c.z_min = c.z_max = p.z;
        } else {
            c.x_min = std::min(c.x_min, p.x);  c.x_max = std::max(c.x_max, p.x);
            c.y_min = std::min(c.y_min, p.y);  c.y_max = std::max(c.y_max, p.y);
            c.z_min = std::min(c.z_min, p.z);  c.z_max = std::max(c.z_max, p.z);
        }

        sx[k] += p.x;  sy[k] += p.y;  sz[k] += p.z;  sd[k] += p.doppler;
        if (p.zone == Zone::DOOR)  n_door[k]++;
        if (p.zone == Zone::CABIN) n_cabin[k]++;
        zs[k].push_back(p.z);
        c.num_points++;
    }

    // 2. 덩어리마다 마무리
    for (int k = 0; k < n_cl; ++k) {
        Cluster& c = out[k];
        c.cluster_id = k;
        if (c.num_points == 0) continue;        // 안전장치 (DBSCAN 이 빈 번호를 주진 않음)

        const double n = static_cast<double>(c.num_points);
        c.cx = static_cast<float>(sx[k] / n);
        c.cy = static_cast<float>(sy[k] / n);
        c.cz = static_cast<float>(sz[k] / n);
        c.mean_doppler = static_cast<float>(sd[k] / n);

        // 가장 많은 zone. 같으면 DOOR (ROI 의 "겹치면 Door 우선" 과 맞춤)
        c.zone = (n_door[k] >= n_cabin[k]) ? Zone::DOOR : Zone::CABIN;

        // z_top: 상위 95% 높이 (가장 가까운 순위 방식)
        // 예: 점 40개 → ceil(38) - 1 = 37번째(0부터) = 위에서 3번째
        std::vector<float>& z = zs[k];
        int idx = static_cast<int>(std::ceil(0.95 * z.size())) - 1;
        idx = std::max(0, std::min(idx, static_cast<int>(z.size()) - 1));
        std::nth_element(z.begin(), z.begin() + idx, z.end());
        c.z_top = z[idx];

        c.maybe_merged = false;                 // 1차: 봉우리 분할 전
    }
    return out;
}