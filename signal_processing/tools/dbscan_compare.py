"""
dbscan_compare.py
DBSCAN 변형 3가지를 같은 점에 돌려 장면별 덩어리 수를 비교한다 (11/26 발표 10장 "사람 덩어리").

  A 기존      : √(Δx² + Δy² + (z_scale·Δz)²) ≤ epsilon  (C++ dbscan.cpp 와 같은 규칙, cfg 값 사용)
  B 격자 이웃 : |Δrange_bin| ≤ a, |Δu_bin| ≤ b, |Δv_bin| ≤ c  (센서 격자 칸 번호, docs/dbscan_design.md 2장)
  C 낮은 병합 : A 결과에서 z_max ≤ low_z 인 '낮은 덩어리'끼리 수평 최근접 거리 ≤ merge_gap 이면 합침
               (누운 사람이 머리·몸통·다리로 쪼개지는 것 복원, design 3장)
  D 봉우리 분할: --split-points 로 peak_split.enabled=1 cfg 로 뽑은 점 CSV 를 주면 그 cluster 칸을 그대로 평가

입력은 radar_pipeline 점 출력 CSV (ROI·배경 제거 뒤 점, cluster 칸 포함)와 같은 이름의 _door.csv(전체 프레임 목록).
격자 칸 번호는 cfg 의 센서 자세로 엘리베이터 좌표를 센서 좌표로 되돌려 polar_grid.cpp 와 같은 식으로 계산한다.

지표 (프레임마다 '유효 덩어리' = 점 min_points 개 이상, A 의 시연 40점 필터와 같은 기준):
  - 맞음 %   : 유효 덩어리 수 == 정답 사람 수 인 프레임 비율
  - 많음 / 적음 % : 정답보다 많음(쪼개짐·유령) / 적음(합쳐짐·놓침)
  - --ghost-zone: 크기(점 수) 대신 위치로 거르기. 중심이 뒷벽 띠(cy ≥ 1.30) 또는 센서 바로 아래(수평 0.4 m 안)이고
    점이 zone_points 미만이면 유효에서 뺌 (ghost_features.py 11/19 결론의 '뒷벽 띠 + 점 60 미만')
  - 적음 중 합쳐짐 % : 적은 프레임 중, 유효 덩어리 하나가 수평으로 wide_m 이상 넓은 경우 (두 사람이 한 덩어리로 묶였다고 봄)
  - --after 초 이후만 따로: 누운 사람이 유효 덩어리로 남는 비율 (S4-4_t01 용)

사용 예 (signal_processing 폴더, .venv 켠 상태):
  python3 tools/dbscan_compare.py --points "data/output/to_C/20261118_recording_3_문짝/S1-5_t03.csv" \
      --cfg config/elevator_4th.cfg --people 2 --label "4차 S1-5_t03" --summary data/output/dbscan_cmp/summary.csv
"""
import argparse
import csv
import os
import re

import numpy as np


# ---------------------------------------------------------------- cfg
def read_cfg(path):
    vals = {}
    with open(path, encoding="utf-8") as fp:
        for line in fp:
            line = line.split("#", 1)[0].strip()
            m = re.match(r"([\w.]+)\s*=\s*(\S+)", line)
            if m:
                try:
                    vals[m.group(1)] = float(m.group(2))
                except ValueError:
                    pass
    return vals


def rotation(roll, pitch, yaw):
    r, p, y = np.radians([roll, pitch, yaw])
    cr, sr, cp, sp, cy, sy = np.cos(r), np.sin(r), np.cos(p), np.sin(p), np.cos(y), np.sin(y)
    return np.array([[cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
                     [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
                     [-sp, cp * sr, cp * cr]])


def grid_bins(xyz, cfg):
    """엘리베이터 좌표 → 센서 좌표 → (range_bin, u_bin, v_bin). polar_grid.cpp 와 같은 식"""
    R = rotation(cfg.get("sensor.roll_deg", 0), cfg.get("sensor.pitch_deg", 0), cfg.get("sensor.yaw_deg", 0))
    t = np.array([cfg.get("sensor.tx", 0), cfg.get("sensor.ty", 0), cfg.get("sensor.tz", 0)])
    s = (xyz - t) @ R          # R^T (e - t) 를 행 벡터로
    r = np.linalg.norm(s, axis=1)
    r = np.where(r > 1e-6, r, 1e-6)
    rr = cfg.get("polar.range_res", 0.071565)
    uv = cfg.get("polar.uv_res", 0.015938)
    return np.stack([np.round(r / rr), np.round(s[:, 0] / r / uv), np.round(s[:, 2] / r / uv)], axis=1)


# ---------------------------------------------------------------- DBSCAN (이웃 행렬 기반)
def dbscan(adj, min_pts):
    """adj: n×n bool (자기 자신 포함). C++ dbscan.cpp 와 같은 순서로 라벨을 매김"""
    n = len(adj)
    labels = np.full(n, -2)
    counts = adj.sum(axis=1)
    cid = 0
    for i in range(n):
        if labels[i] != -2:
            continue
        if counts[i] < min_pts:
            labels[i] = -1
            continue
        labels[i] = cid
        queue = list(np.flatnonzero(adj[i]))
        k = 0
        while k < len(queue):
            j = queue[k]
            k += 1
            if labels[j] == -1:
                labels[j] = cid
                continue
            if labels[j] != -2:
                continue
            labels[j] = cid
            if counts[j] >= min_pts:
                queue.extend(np.flatnonzero(adj[j]))
        cid += 1
    return labels


def adj_euclid(xyz, eps, zs):
    d = xyz[:, None, :] - xyz[None, :, :]
    d[..., 2] *= zs
    return (d ** 2).sum(-1) <= eps * eps


def adj_grid(bins, a, b, c):
    d = np.abs(bins[:, None, :] - bins[None, :, :])
    return (d[..., 0] <= a) & (d[..., 1] <= b) & (d[..., 2] <= c)


def merge_low(xyz, labels, low_z, gap, min_low):
    """z_max ≤ low_z 인 덩어리끼리 수평 최근접 거리 ≤ gap 이면 Union-Find 로 합침"""
    ids = [c for c in np.unique(labels) if c >= 0]
    low = [c for c in ids if xyz[labels == c, 2].max() <= low_z and (labels == c).sum() >= min_low]
    parent = {c: c for c in ids}

    def find(c):
        while parent[c] != c:
            parent[c] = parent[parent[c]]
            c = parent[c]
        return c

    for i, ca in enumerate(low):
        pa = xyz[labels == ca, :2]
        for cb in low[i + 1:]:
            pb = xyz[labels == cb, :2]
            dmin = np.sqrt(((pa[:, None, :] - pb[None, :, :]) ** 2).sum(-1).min())
            if dmin <= gap:
                parent[find(cb)] = find(ca)
    out = labels.copy()
    for c in ids:
        out[labels == c] = find(c)
    return out


# ---------------------------------------------------------------- 입력
def read_points(path):
    rows = {}
    with open(path, newline="") as fp:
        for row in csv.DictReader(fp):
            f = int(row["frame"])
            rows.setdefault(f, []).append((int(row["timestamp_us"]), float(row["x"]), float(row["y"]),
                                           float(row["z"]), int(row.get("cluster", -1) or -1)))
    return rows


def read_frames(door_path):
    out = []
    with open(door_path, newline="") as fp:
        for row in csv.DictReader(fp):
            out.append((int(row["frame_id"]), int(row["timestamp_us"])))
    return out


def door_path_of(points_path):
    base, ext = os.path.splitext(points_path)
    return base + "_door" + ext


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--points", required=True, help="radar_pipeline 점 출력 CSV (같은 폴더에 _door.csv 필요)")
    ap.add_argument("--cfg", required=True, help="그 점을 뽑을 때 쓴 cfg (센서 자세·DBSCAN 값)")
    ap.add_argument("--people", type=int, required=True, help="정답 사람 수")
    ap.add_argument("--label", default="")
    ap.add_argument("--min-points", type=int, default=40, help="유효 덩어리 기준 (A 시연 필터)")
    ap.add_argument("--grid", default="4,8,12", help="B 격자 이웃 a,b,c (거리·좌우·위아래 칸 수)")
    ap.add_argument("--low-z", type=float, default=0.6, help="C 낮은 덩어리 기준 z_max [m]")
    ap.add_argument("--merge-gap", type=float, default=0.5, help="C 병합 수평 거리 [m]")
    ap.add_argument("--after", type=float, default=-1, help="이 시각(초) 이후만 따로 집계 (누운 구간)")
    ap.add_argument("--before", type=float, default=1e9, help="--after 구간의 끝 (초)")
    ap.add_argument("--summary", default="", help="결과를 덧붙일 CSV")
    ap.add_argument("--split-points", default="", help="D: peak_split.enabled=1 로 뽑은 같은 장면 점 CSV")
    ap.add_argument("--ghost-zone", action="store_true", help="위치 기반 거르기 켜기")
    ap.add_argument("--zone-points", type=int, default=60, help="위치 거르기: 이 점 수 미만만 뺌")
    ap.add_argument("--wide-m", type=float, default=0.7, help="합쳐짐 판단 수평 폭 [m]")
    a = ap.parse_args()

    cfg = read_cfg(a.cfg)
    eps = cfg.get("dbscan.epsilon", 0.30)
    zs = cfg.get("dbscan.z_scale", 0.2)
    mp = int(cfg.get("dbscan.min_pts", 10))
    ga, gb, gc = (int(v) for v in a.grid.split(","))

    pts = read_points(a.points)
    frames = read_frames(door_path_of(a.points))
    t0 = frames[0][1]

    split = read_points(a.split_points) if a.split_points else None
    names = ["A 기존", "B 격자 이웃", "C 낮은 병합"] + (["D 봉우리 분할"] if split else [])
    merged = {k: [] for k in names}     # 프레임별: 넓은 유효 덩어리가 있는지
    valid = {k: [] for k in names}      # 프레임별 유효 덩어리 수
    allc = {k: [] for k in names}       # 프레임별 전체 덩어리 수
    tsec = []
    mismatch = 0                        # A 재현 확인: C++ cluster 칸과 덩어리 수가 다른 프레임

    for f, ts in frames:
        tsec.append((ts - t0) / 1e6)
        rows = pts.get(f, [])
        if not rows:
            for k in names:
                valid[k].append(0)
                allc[k].append(0)
                merged[k].append(False)
            continue
        arr = np.array(rows, dtype=float)
        xyz = arr[:, 1:4]
        cpp = arr[:, 4].astype(int)

        la = dbscan(adj_euclid(xyz, eps, zs), mp)
        lb = dbscan(adj_grid(grid_bins(xyz, cfg), ga, gb, gc), mp)
        lc = merge_low(xyz, la, a.low_z, a.merge_gap, 5)
        if len(set(la[la >= 0])) != len(set(cpp[cpp >= 0])):
            mismatch += 1

        labs = [la, lb, lc]
        if split:
            srows = split.get(f, [])
            ls = np.array([r[4] for r in srows]) if len(srows) == len(rows) else la
            labs.append(ls)
        for k, lab in zip(names, labs):
            ids, cnt = np.unique(lab[lab >= 0], return_counts=True)
            allc[k].append(len(ids))
            keep = cnt >= a.min_points
            if a.ghost_zone:
                for i, c in enumerate(ids):
                    q = xyz[lab == c]
                    cx, cy = q[:, 0].mean(), q[:, 1].mean()
                    in_zone = cy >= 1.30 or np.hypot(cx + 0.65, cy - 1.31) <= 0.4
                    if in_zone and cnt[i] < a.zone_points:
                        keep[i] = False
            big = ids[keep]
            valid[k].append(len(big))
            wide = False
            for c in big:
                q = xyz[lab == c]
                if max(np.ptp(q[:, 0]), np.ptp(q[:, 1])) >= a.wide_m:
                    wide = True
            merged[k].append(wide)

    tsec = np.array(tsec)
    label = a.label or os.path.basename(a.points)
    print(f"[{label}] 프레임 {len(frames)}개, 정답 {a.people}명, 유효 덩어리 = 점 {a.min_points}개 이상")
    print(f"  A 재현 확인: C++ 결과와 덩어리 수가 다른 프레임 {mismatch}개 (CSV 소수점 3자리 반올림 때문에 몇 개는 생길 수 있음)")
    if a.ghost_zone:
        print(f"  위치 거르기 켜짐: 뒷벽 띠·센서 아래 + 점 {a.zone_points}개 미만 제외")
    print(f"  B 격자 이웃 a,b,c = {ga},{gb},{gc} / C 낮은 병합 z_max ≤ {a.low_z} m, 간격 ≤ {a.merge_gap} m")

    sel_after = ((tsec >= a.after) & (tsec < a.before)) if a.after >= 0 else None
    head = f"  {'방식':12s} {'전체 덩어리/프레임':>16s} {'유효/프레임':>10s} {'맞음':>7s} {'많음':>7s} {'적음':>7s} {'적음 중 합쳐짐':>12s}"
    if sel_after is not None:
        head += f"  {a.after:.0f}~{min(a.before, tsec[-1]):.0f}초 유효≥1"
    print(head)
    rows_out = []
    for k in names:
        v = np.array(valid[k])
        right = np.mean(v == a.people) * 100
        more = np.mean(v > a.people) * 100
        less = np.mean(v < a.people) * 100
        under = v < a.people
        mpct = np.mean(np.array(merged[k])[under]) * 100 if under.any() else 0.0
        line = f"  {k:12s} {np.mean(allc[k]):16.2f} {v.mean():10.2f} {right:6.1f}% {more:6.1f}% {less:6.1f}% {mpct:11.1f}%"
        keep = ""
        if sel_after is not None and sel_after.any():
            keep = f"{np.mean(v[sel_after] >= 1) * 100:.1f}"
            line += f"  {keep:>10s}%"
        print(line)
        rows_out.append([label, a.people, k, f"{np.mean(allc[k]):.2f}", f"{v.mean():.2f}",
                         f"{right:.1f}", f"{more:.1f}", f"{less:.1f}", f"{mpct:.1f}", keep, a.grid, a.low_z, a.merge_gap])

    if a.summary:
        new = not os.path.exists(a.summary)
        os.makedirs(os.path.dirname(a.summary) or ".", exist_ok=True)
        with open(a.summary, "a", newline="") as fp:
            w = csv.writer(fp)
            if new:
                w.writerow(["scene", "people", "method", "clusters_per_frame", "valid_per_frame",
                            "right_pct", "more_pct", "less_pct", "less_merged_pct", "after_keep_pct", "grid_abc", "low_z", "merge_gap"])
            w.writerows(rows_out)
        print("  요약 덧붙임:", a.summary)


if __name__ == "__main__":
    main()
