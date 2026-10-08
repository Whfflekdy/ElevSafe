// background_subtractor.h
// [Step 2] 정적 배경 제거 (극좌표 격자 배경 맵)
//
// 빈 장면의 프레임들에서 "자주 점이 찍히는 격자 칸"을 배경으로 등록하고,
// 이후 프레임에서 배경 칸에 떨어진 점을 제거한다.
// 격자 번호(has_bins, range_bin, u_bin, v_bin)가 계산된 점만 판정한다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include <string>

#include "common/config.h"
#include "common/radar_point.h"

struct BackgroundConfig {
    float occupancy_threshold = 0.3f;  // 전체 프레임 중 이 비율 이상 점이 찍힌 칸 = 배경
    int dilation = 0;                  // 배경 칸 주변 몇 칸까지 배경으로 넓힐지 (0 = 넓히지 않음)

    static BackgroundConfig fromConfig(const Config& cfg);
};

struct BGStats {
    std::size_t input = 0;
    std::size_t removed = 0;
};

class BackgroundSubtractor {
public:
    explicit BackgroundSubtractor(const BackgroundConfig& config);

    // 빈 장면 프레임들로 배경 맵 생성 (이전 맵은 지워짐)
    void build(const std::vector<FrameData>& empty_frames);

    // 배경 칸에 떨어진 점을 제거한 결과 반환
    Frame filter(const Frame& points, BGStats* stats = nullptr) const;

    std::size_t backgroundCellCount() const { return background_.size(); }

    // 칸 번호 세 개 -> 열쇠 하나
    static std::int64_t makeKey(int range_bin, int u_bin, int v_bin);

    // 배경 맵 파일 저장 / 불러오기 (성공 시 true)
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    // 열쇠 하나 -> 칸 번호 세 개 (makeKey 의 반대)
    static void splitKey(std::int64_t key, int& range_bin, int& u_bin, int& v_bin);

private:
    BackgroundConfig config_;
    std::unordered_set<std::int64_t> background_;   // 배경으로 등록된 칸들의 열쇠
};