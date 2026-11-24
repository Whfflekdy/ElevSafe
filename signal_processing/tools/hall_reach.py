"""승강장 쪽(cy < -0.3) 덩어리가 y별로 얼마나 잡히는지.
사용: python tools/hall_reach.py <_clusters.csv> [<_clusters.csv> ...]
"""
import csv, sys
from collections import defaultdict

MIN_PTS = 15            # 먼 곳은 점이 적을 수 있어 40보다 낮게
bins = defaultdict(lambda: [0, 0, [], []])   # y칸 -> [덩어리 수, 점 합, cx들, z_top들]
frames = set()
for path in sys.argv[1:]:
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            frames.add((path, r["frame_id"]))
            if not r.get("cluster_id"):
                continue
            cy, n = float(r["cy"]), int(r["num_points"])
            if cy >= -0.3 or n < MIN_PTS:
                continue
            k = int(cy / 0.25 - 1) * 0.25          # 0.25 m 칸 (-0.50 ~ -0.25 → -0.50)
            b = bins[k]
            b[0] += 1; b[1] += n
            b[2].append(float(r["cx"])); b[3].append(float(r["z_top"]))

print(f"파일 {len(sys.argv) - 1}개, 프레임 {len(frames)}개, 점 {MIN_PTS}개 이상 덩어리만")
print(f"{'y 칸':>12}{'덩어리':>7}{'평균 점':>8}{'cx 범위':>16}{'z_top 중앙':>11}")
for k in sorted(bins, reverse=True):
    c, s, xs, zs = bins[k]
    xs.sort(); zs.sort()
    print(f"{k:>6.2f}~{k + 0.25:>5.2f}{c:>7}{s / c:>8.0f}"
          f"{xs[len(xs) // 20]:>8.2f}~{xs[-1 - len(xs) // 20]:>5.2f}{zs[len(zs) // 2]:>11.2f}")