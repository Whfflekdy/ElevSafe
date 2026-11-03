#!/usr/bin/env python3
"""봉우리로 나눈 두 덩어리가 사람 간격으로 떨어져 있는지 확인한다.

사용법: python3 tools/check_split_distance.py <출력.csv>
<출력.csv> 는 radar_pipeline 에 준 출력 이름. 옆의 _clusters.csv 를 읽는다.
"""
import csv
import math
import sys
from collections import defaultdict


def pct(values, p):
    return values[min(len(values) - 1, int(len(values) * p / 100))]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    base = sys.argv[1][:-4] if sys.argv[1].endswith(".csv") else sys.argv[1]

    frames = defaultdict(list)
    all_frames = set()
    with open(base + "_clusters.csv", newline="") as f:
        for row in csv.DictReader(f):
            all_frames.add(row["frame_id"])
            if row.get("cx"):
                frames[row["frame_id"]].append(row)

    size = defaultdict(int)
    for frame_id in all_frames:
        size[len(frames[frame_id])] += 1
    print("프레임별 덩어리 수:", ", ".join(f"{k}개 {size[k]}프레임" for k in sorted(size)))

    gaps = []
    points = []
    for rows in frames.values():
        split = [r for r in rows if r["maybe_merged"] == "1"]
        if len(split) != 2:
            continue
        a, b = split
        gaps.append(math.hypot(float(a["cx"]) - float(b["cx"]), float(a["cy"]) - float(b["cy"])))
        points.extend(int(r["num_points"]) for r in split)

    if not gaps:
        print("maybe_merged 덩어리 2개인 프레임 없음")
        return
    gaps.sort()
    points.sort()
    print(f"나뉜 프레임 {len(gaps)}개")
    print(f"중심 간격 m: 최저 {gaps[0]:.2f}, 5% {pct(gaps, 5):.2f}, 중앙 {pct(gaps, 50):.2f}, "
          f"95% {pct(gaps, 95):.2f}, 최고 {gaps[-1]:.2f}")
    print(f"조각 점 수: 최저 {points[0]}, 중앙 {pct(points, 50)}, 최고 {points[-1]}")


if __name__ == "__main__":
    main()
