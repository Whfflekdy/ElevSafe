"""
door_static_probe.py
문 상태를 '정지 반사'로 가를 수 있는지 보는 분석 도구 (11/24, 문짝 도플러 + 정지 반사 결합 준비).

결론 (11/24, 3차 S3-7_t01 타이머 기준 문 6회 + S4-4_t01 문 닫힘·넘어짐):
  - (가) 문 평면 정지 점은 문이 닫혀 있어도 프레임당 0.1개 → 가르기 51%. 닫힌 문짝이 레이더에 거의 안 찍힘
    (평평한 금속 문을 비스듬히 봐서 반사가 센서로 안 돌아오는 것으로 추정)
  - (나) 벽 너머 64%, (다) 문 너머 보기 65%: 문이 아니라 승객 위치를 따라감
    (하차 회차는 (다)만 높고, 탑승 회차는 (나)만 높음)
  - 문짝 움직임 점 수만 문 움직임 12회와 정확히 겹침 → 정지 반사 결합은 하지 않음. 첫 움직임 전 UNKNOWN 유지

입력은 *_wide.cfg 로 돌린 점 출력 CSV (좌표 변환만, 구역·배경 제거 없음).
프레임마다 아래 값을 세서 시간축 그래프와 상태별 요약을 낸다.

  motion  : 문짝 움직임 상자(x ±0.60, y −0.18~+0.27, z 1.80~2.40)의 |도플러| ≥ 0.05 점 수
            (door_state_estimator 와 같은 상자. 10프레임 합 ≥ 100 이면 움직이는 중)
  (가) door_static : 문 평면 정지 점. x ±0.44, y −0.18~+0.18, z 0.30~2.00, |도플러| < 0.05
  (나) beyond      : 캐빈 벽 너머 정지 점 (LDI 근사). 옆벽 |x| > 0.91 (y 0.12~1.49) 또는 뒷벽 y > 1.59
  (다) view        : 기존 '문 너머 보기' 상자 점 수. x ±0.44, y −2.88~−0.18, z 0.10~2.10 (도플러 무관)

기준 상태(색 띠, 요약 기준)는 둘 중 하나:
  --truth "0:CLOSED,4.8:OPENING,7.5:OPEN,45:CLOSING,48.5:CLOSED"   (타이머 기록, 초)
  --door  <장면>_door.csv                                          (B 문짝 방식 출력. 정답이 아님)
움직이는 중(10프레임 motion 합 ≥ 30, 또는 OPENING/CLOSING)은 요약에서 뺀다.

사용 예 (signal_processing 폴더, .venv 켠 상태):
  python3 tools/door_static_probe.py --wide data/output/probe/4th_S0-2_t06_wide.csv \
      --door data/output/probe/4th_S0-2_t06_door.csv --label "4차 S0-2_t06" \
      --save data/output/probe/4th_S0-2_t06_probe.png --summary data/output/probe/summary.csv
  닫힌 문 장면: --truth "0:CLOSED"
"""
import argparse
import csv
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from matplotlib import font_manager
_have = {f.name for f in font_manager.fontManager.ttflist}
plt.rcParams["font.family"] = [f for f in ("AppleGothic", "Malgun Gothic", "NanumGothic",
                               "Noto Sans CJK JP", "Noto Sans CJK KR") if f in _have] + ["DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

MIN_DOP = 0.05
WINDOW = 10            # 0.5초 (20 Hz)
MOVING_SUM = 30        # 10프레임 motion 합이 이 이상이면 '움직이는 중'으로 보고 요약에서 뺌

# 상자: (x_min, x_max, y_min, y_max, z_min, z_max)
BOX_MOTION = (-0.60, 0.60, -0.18, 0.27, 1.80, 2.40)
BOX_DOOR = (-0.44, 0.44, -0.18, 0.18, 0.30, 2.00)
BOX_VIEW = (-0.44, 0.44, -2.88, -0.18, 0.10, 2.10)
CABIN_SIDE_X, CABIN_Y, BACK_Y = 0.91, (0.12, 1.49), 1.59
BEYOND_Z = (0.10, 2.40)

FEATURES = [("motion", "문짝 움직임 점 수 (|v|≥0.05)"),
            ("door_n", "(가) 문 평면 정지 점 수"),
            ("door_p", "(가) 문 평면 정지 세기 합"),
            ("beyond_n", "(나) 벽 너머 정지 점 수"),
            ("view_n", "(다) 문 너머 보기 점 수 (기존)")]
STATE_COLOR = {"OPEN": "#b7e4c7", "CLOSED": "#f4c2c2", "OPENING": "#fde68a",
               "CLOSING": "#fde68a", "UNKNOWN": "#e5e7eb"}


def in_box(x, y, z, b):
    return (x >= b[0]) & (x <= b[1]) & (y >= b[2]) & (y <= b[3]) & (z >= b[4]) & (z <= b[5])


def read_points(path):
    cols = {k: [] for k in ("frame", "t", "x", "y", "z", "d", "p")}
    with open(path, newline="") as fp:
        for row in csv.DictReader(fp):
            cols["frame"].append(int(row["frame"]))
            cols["t"].append(int(row["timestamp_us"]) / 1e6)
            cols["x"].append(float(row["x"]))
            cols["y"].append(float(row["y"]))
            cols["z"].append(float(row["z"]))
            cols["d"].append(float(row["doppler"]))
            cols["p"].append(float(row["power"]))
    return {k: np.array(v) for k, v in cols.items()}


def per_frame(pts):
    """프레임별 값. 점이 0개인 프레임은 점 CSV에 없으므로 B 문 CSV 쪽 프레임과 맞출 때 0으로 채움"""
    x, y, z, d, p = pts["x"], pts["y"], pts["z"], pts["d"], pts["p"]
    still = np.abs(d) < MIN_DOP
    m_motion = in_box(x, y, z, BOX_MOTION) & ~still
    m_door = in_box(x, y, z, BOX_DOOR) & still
    side = (np.abs(x) > CABIN_SIDE_X) & (y >= CABIN_Y[0]) & (y <= CABIN_Y[1])
    back = y > BACK_Y
    m_beyond = (side | back) & still & (z >= BEYOND_Z[0]) & (z <= BEYOND_Z[1])
    m_view = in_box(x, y, z, BOX_VIEW)

    frames, idx = np.unique(pts["frame"], return_inverse=True)
    n = len(frames)
    t = np.zeros(n)
    t[idx] = pts["t"]
    out = {"frame": frames, "t": t}
    for name, mask, w in (("motion", m_motion, None), ("door_n", m_door, None),
                          ("door_p", m_door, p), ("beyond_n", m_beyond, None), ("view_n", m_view, None)):
        weights = mask.astype(float) if w is None else np.where(mask, w, 0.0)
        out[name] = np.bincount(idx, weights=weights, minlength=n)
    return out


def rolling(v, w=WINDOW, how="mean"):
    c = np.cumsum(np.insert(v, 0, 0.0))
    s = np.empty_like(v, dtype=float)
    for i in range(len(v)):
        lo = max(0, i - w + 1)
        s[i] = c[i + 1] - c[lo]
        if how == "mean":
            s[i] /= (i + 1 - lo)
    return s


def ref_from_truth(t, spec):
    marks = []
    for item in spec.split(","):
        sec, st = item.split(":")
        marks.append((float(sec), st.strip().upper()))
    marks.sort()
    ref = np.array(["UNKNOWN"] * len(t), dtype=object)
    for sec, st in marks:
        ref[t >= sec] = st
    return ref


def ref_from_door(frames, path):
    m = {}
    with open(path, newline="") as fp:
        for row in csv.DictReader(fp):
            m[int(row["frame_id"])] = row["state"]
    return np.array([m.get(int(f), "UNKNOWN") for f in frames], dtype=object)


def best_split(a, b):
    """OPEN 값 a, CLOSED 값 b 를 문턱 하나로 가를 때 가장 좋은 균형 정확도와 문턱, 방향"""
    if len(a) == 0 or len(b) == 0:
        return None
    vals = np.unique(np.concatenate([a, b]))
    best = (0.0, 0.0, "")
    for th in vals:
        acc_hi = 0.5 * (np.mean(a > th) + np.mean(b <= th))   # 열림이 더 큼
        acc_lo = 0.5 * (np.mean(a <= th) + np.mean(b > th))   # 닫힘이 더 큼
        if acc_hi > best[0]:
            best = (acc_hi, th, "OPEN>")
        if acc_lo > best[0]:
            best = (acc_lo, th, "CLOSED>")
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wide", required=True, help="*_wide.cfg 점 출력 CSV")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--truth", help='타이머 기록. 예: "0:CLOSED,4.8:OPENING,7.5:OPEN"')
    g.add_argument("--door", help="B 문짝 방식 _door.csv (정답 대신 참고)")
    ap.add_argument("--label", default="", help="그림 제목·요약에 쓸 장면 이름")
    ap.add_argument("--save", default="", help="그림 저장 경로 (없으면 그림 안 그림)")
    ap.add_argument("--summary", default="", help="상태별 요약을 덧붙일 CSV (여러 장면 모으기)")
    a = ap.parse_args()

    pf = per_frame(read_points(a.wide))
    t = pf["t"] - pf["t"][0]
    ref = ref_from_truth(t, a.truth) if a.truth else ref_from_door(pf["frame"], a.door)
    moving = rolling(pf["motion"], how="sum") >= MOVING_SUM
    smooth = {k: rolling(pf[k]) for k, _ in FEATURES}

    label = a.label or os.path.basename(a.wide)
    src = "타이머" if a.truth else "B 문짝 방식"
    print(f"[{label}] 프레임 {len(t)}개, {t[-1]:.1f}초, 기준 = {src}")
    steady = {}
    for st in ("OPEN", "CLOSED"):
        steady[st] = (ref == st) & ~moving
        print(f"  {st:6s} 정지 구간 {steady[st].sum():5d}프레임")

    rows = []
    print(f"  {'값':24s} {'OPEN 평균(p10~p90)':>24s} {'CLOSED 평균(p10~p90)':>24s}  가르기")
    for k, name in FEATURES:
        cells = []
        for st in ("OPEN", "CLOSED"):
            v = smooth[k][steady[st]]
            if len(v):
                cells.append(f"{v.mean():7.1f} ({np.percentile(v, 10):5.1f}~{np.percentile(v, 90):5.1f})")
                rows.append([label, src, st, k, len(v), f"{v.mean():.2f}", f"{np.percentile(v, 10):.2f}",
                             f"{np.median(v):.2f}", f"{np.percentile(v, 90):.2f}"])
            else:
                cells.append("-")
        sp = best_split(smooth[k][steady["OPEN"]], smooth[k][steady["CLOSED"]])
        sp_txt = f"{sp[0] * 100:5.1f}% ({sp[2]}{sp[1]:.1f})" if sp else "한쪽 상태만 있음"
        print(f"  {name:24s} {cells[0]:>24s} {cells[1]:>24s}  {sp_txt}")

    if a.summary:
        new = not os.path.exists(a.summary)
        os.makedirs(os.path.dirname(a.summary) or ".", exist_ok=True)
        with open(a.summary, "a", newline="") as fp:
            w = csv.writer(fp)
            if new:
                w.writerow(["scene", "ref", "state", "feature", "frames", "mean", "p10", "p50", "p90"])
            w.writerows(rows)
        print("  요약 덧붙임:", a.summary)

    if a.save:
        fig, axes = plt.subplots(len(FEATURES), 1, figsize=(13, 2.2 * len(FEATURES)), sharex=True)
        edges = np.flatnonzero(ref[1:] != ref[:-1]) + 1
        starts = np.concatenate([[0], edges])
        ends = np.concatenate([edges, [len(t)]])
        for ax, (k, name) in zip(axes, FEATURES):
            for s, e in zip(starts, ends):
                ax.axvspan(t[s], t[e - 1], color=STATE_COLOR.get(ref[s], "#ffffff"), alpha=0.6, lw=0)
            ax.plot(t, pf[k], color="0.75", lw=0.6)
            ax.plot(t, smooth[k], color="k", lw=1.3)
            ax.set_ylabel(name, fontsize=9)
            ax.grid(alpha=0.3)
        axes[-1].set_xlabel("시간 (초, 첫 프레임 0)")
        handles = [plt.Rectangle((0, 0), 1, 1, color=STATE_COLOR[s]) for s in ("OPEN", "CLOSED", "OPENING", "UNKNOWN")]
        axes[0].legend(handles, ["OPEN", "CLOSED", "움직이는 중", "UNKNOWN"], loc="upper right", ncol=4, fontsize=8)
        fig.suptitle(f"문 정지 반사 분석: {label} (기준 = {src}, 검은 선 = 0.5초 평균)")
        fig.tight_layout()
        os.makedirs(os.path.dirname(a.save) or ".", exist_ok=True)
        fig.savefig(a.save, dpi=130)
        print("  그림:", a.save)


if __name__ == "__main__":
    main()
