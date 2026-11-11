#!/usr/bin/env python3
"""덩어리가 2개 이상인 프레임에서 유령 후보를 정리한다.

사용법: python3 tools/check_extra_clusters.py <출력.csv> [--skip x y]
<출력.csv> 는 radar_pipeline 에 준 출력 이름. 옆의 _clusters.csv 를 읽는다.
장면에 사람이 1명뿐일 때, 프레임에서 점이 가장 많은 덩어리를 사람으로 보고
나머지를 유령 후보로 모은다.
--skip x y: 그 위치 반경 0.3 m 안의 후보를 고정 물체로 보고 뺀다.

  가까움: 사람 중심에서 0.6 m 안 (한 사람이 쪼개졌을 가능성)
  멀리:   0.6 m 넘게 떨어짐 (유령 가능성)
"""
import csv
import math
import sys
from collections import Counter, defaultdict


def main():
    args = sys.argv[1:]
    skip = None
    if "--skip" in args:
        i = args.index("--skip")
        skip = (float(args[i + 1]), float(args[i + 2]))
        del args[i:i + 3]
    if len(args) != 1:
        sys.exit(__doc__)
    base = args[0][:-4] if args[0].endswith(".csv") else args[0]

    frames = defaultdict(list)
    order = []
    with open(base + "_clusters.csv", newline="") as f:
        for row in csv.DictReader(f):
            if row["frame_id"] not in frames:
                order.append(row["frame_id"])
            frames[row["frame_id"]]
            if row.get("cx"):
                frames[row["frame_id"]].append(row)

    size = Counter(len(rows) for rows in frames.values())
    print(f"전체 {len(frames)}프레임:", ", ".join(f"{k}개 {size[k]}프레임" for k in sorted(size)))

    spots = Counter()
    groups = defaultdict(list)
    near = far = skipped = 0
    flags = []
    for frame_id in order:
        rows = sorted(frames[frame_id], key=lambda r: int(r["num_points"]), reverse=True)
        has_extra = False
        for r in rows[1:]:
            x, y = float(r["cx"]), float(r["cy"])
            if skip and math.dist((x, y), skip) <= 0.3:
                skipped += 1
                continue
            has_extra = True
            d = math.dist((x, y), (float(rows[0]["cx"]), float(rows[0]["cy"])))
            if d <= 0.6:
                near += 1
            else:
                far += 1
            key = (round(x / 0.2) * 0.2, round(y / 0.2) * 0.2)
            spots[key] += 1
            groups[key].append((r, d))
        flags.append(has_extra)

    runs = Counter()
    run = 0
    for flag in flags + [False]:
        if flag:
            run += 1
        elif run:
            runs[run] += 1
            run = 0

    print(f"유령 후보 {near + far}개 (가까움 {near}, 멀리 {far}), 고정 물체로 뺀 것 {skipped}개")
    if runs:
        print(f"연속 지속: 최장 {max(runs)}프레임, 1프레임만 나온 경우 {runs[1]}번, 전체 {sum(runs.values())}번")
    print("   x      y    횟수  높이 z_top  점 수  도플러  사람과 거리")
    for key, count in spots.most_common(10):
        rows = groups[key]
        z = sum(float(r["z_top"]) for r, _ in rows) / count
        n = sum(int(r["num_points"]) for r, _ in rows) / count
        v = sum(float(r["mean_doppler"]) for r, _ in rows) / count
        d = sum(dist for _, dist in rows) / count
        print(f"{key[0]:+.1f}  {key[1]:+.1f}  {count:5d}  {z:9.2f}  {n:5.0f}  {v:+.2f}  {d:9.2f}")


if __name__ == "__main__":
    main()