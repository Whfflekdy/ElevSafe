"""
plot_elevator_s0.py
1차 엘리베이터 S0(빈 캐빈) 결과를 그림으로 저장한다.

  1) door_state.png : S0-3 시간에 따른 문 너머 점 수 + OPEN/CLOSED 구간
  2) topview.png    : 빈 캐빈을 위에서 본 점 분포 (캐빈·문·센서·환풍기 표시)
  3) sideview.png   : 빈 캐빈을 옆에서 본 점 분포 (y-z)

입력 (signal_processing 폴더 기준, 앞에서 만든 파일)
  - 문 상태: elevator_1st_wide.cfg 로 돌린 S0-3 점 출력(_wide.csv) + elevator_1st.cfg 로 돌린 _door.csv
  - 장면:   elevator_1st_wide.cfg 로 돌린 S0-1_t02 점 출력(_wide.csv)

사용법 (signal_processing 폴더에서, .venv 켠 상태):
  python3 tools/plot_elevator_s0.py
  python3 tools/plot_elevator_s0.py --out-dir data/output/figures
"""
import argparse
import csv
import os
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")                     # 화면 없이 파일로만 저장
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
from matplotlib.colors import LogNorm

# 한글 글꼴 (Mac 기본). 없으면 영어 글꼴로 넘어가며 한글이 네모로 보일 수 있음
plt.rcParams["font.family"] = ["AppleGothic", "Malgun Gothic", "NanumGothic", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

# ---- 1차 엘리베이터 치수 (config/elevator_1st.cfg 와 같게) ----
CABIN_W, CABIN_D = 1.62, 1.37            # 폭(x), 깊이(y)
DOOR_W, DOOR_BAND = 0.88, 0.24           # 문 폭, 문 띠 깊이 (y −0.24 ~ 0)
SENSOR = (-0.65, 1.19, 2.14)             # 센서 위치 (x, y, z)
DOOR_VIEW = dict(x=(-0.44, 0.44), y=(-3.0, -0.30), z=(0.10, 2.10))   # 문 상태 추정 상자
OPEN_TH, CLOSE_TH, WINDOW = 10, 0.5, 10


def read_points(path):
    """점 출력 CSV → frame, t(초), x, y, z, doppler 배열"""
    f, t, x, y, z, d = [], [], [], [], [], []
    with open(path, newline="") as fp:
        r = csv.DictReader(fp)
        for row in r:
            f.append(int(row["frame"]))
            t.append(int(row["timestamp_us"]) / 1e6)
            x.append(float(row["x"])); y.append(float(row["y"])); z.append(float(row["z"]))
            d.append(float(row["doppler"]))
    return [np.array(v) for v in (f, t, x, y, z, d)]


def plot_door_state(wide_csv, door_csv, out_png):
    f, t, x, y, z, _ = read_points(wide_csv)

    # 프레임마다 문 너머 상자 안 점 수
    inside = ((x > DOOR_VIEW["x"][0]) & (x < DOOR_VIEW["x"][1]) &
              (y > DOOR_VIEW["y"][0]) & (y < DOOR_VIEW["y"][1]) &
              (z > DOOR_VIEW["z"][0]) & (z < DOOR_VIEW["z"][1]))
    frame_t, count = {}, defaultdict(int)
    for fi, ti, ok in zip(f, t, inside):
        frame_t[fi] = ti
        if ok:
            count[fi] += 1
    frames = sorted(frame_t)
    ts = np.array([frame_t[k] for k in frames])
    cs = np.array([count[k] for k in frames], dtype=float)
    avg = np.convolve(cs, np.ones(WINDOW) / WINDOW, mode="full")[:len(cs)]   # 최근 WINDOW 프레임 평균

    # 추정 결과 (OPEN 구간 색칠용)
    st_t, st = [], []
    with open(door_csv, newline="") as fp:
        for row in csv.DictReader(fp):
            st_t.append(int(row["timestamp_us"]) / 1e6)
            st.append(row["state"])

    fig, ax = plt.subplots(figsize=(11, 4))
    start = None
    for i, s in enumerate(st + ["END"]):
        if s == "OPEN" and start is None:
            start = st_t[i]
        elif s != "OPEN" and start is not None:
            end = st_t[i] if i < len(st_t) else st_t[-1]
            ax.axvspan(start, end, color="tab:green", alpha=0.15, lw=0)
            start = None
    ax.plot(ts, cs, color="0.7", lw=0.8, label="프레임별 점 수")
    ax.plot(ts, avg, color="tab:blue", lw=1.8, label=f"최근 {WINDOW}프레임 평균")
    ax.axhline(OPEN_TH, color="tab:green", ls="--", lw=1, label=f"열림 기준 {OPEN_TH}")
    ax.axhline(CLOSE_TH, color="tab:red", ls="--", lw=1, label=f"닫힘 기준 {CLOSE_TH}")
    ax.set_xlabel("시간 (초)")
    ax.set_ylabel("문 너머 점 수 (점/프레임)")
    ax.set_title("레이더 문 상태 추정 — S0-3 문 개폐 반복 (초록 구간 = OPEN 판정)")
    ax.legend(loc="upper right", fontsize=8, ncol=2)
    ax.set_xlim(ts[0], ts[-1])
    ax.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)
    print("저장:", out_png)


def draw_cabin_top(ax):
    ax.add_patch(Rectangle((-CABIN_W / 2, 0), CABIN_W, CABIN_D, fill=False, lw=1.5, ec="k"))
    ax.add_patch(Rectangle((-DOOR_W / 2, -DOOR_BAND), DOOR_W, DOOR_BAND, fill=False, lw=1.2,
                           ec="tab:orange", ls="--"))
    ax.add_patch(Rectangle((DOOR_VIEW["x"][0], DOOR_VIEW["y"][0]),
                           DOOR_VIEW["x"][1] - DOOR_VIEW["x"][0], DOOR_VIEW["y"][1] - DOOR_VIEW["y"][0],
                           fill=False, lw=1, ec="tab:green", ls=":"))
    ax.plot(SENSOR[0], SENSOR[1], marker="*", ms=14, color="red", mec="k")
    ax.annotate("센서", (SENSOR[0], SENSOR[1]), xytext=(6, 6), textcoords="offset points", fontsize=9)
    ax.text(0, CABIN_D / 2, "캐빈", ha="center", fontsize=10, color="0.3")
    ax.text(0, -DOOR_BAND / 2, "문", ha="center", va="center", fontsize=8, color="tab:orange")
    ax.text(0, -1.6, "문 상태 추정 상자", ha="center", fontsize=8, color="tab:green")


def plot_topview(wide_csv, out_png):
    f, t, x, y, z, d = read_points(wide_csv)
    n_frames = len(set(f.tolist()))
    high = z > 2.1                                # 천장 높이 (환풍기)

    fig, ax = plt.subplots(figsize=(6.5, 7.5))
    h = ax.hist2d(x[~high], y[~high], bins=[np.arange(-2, 2.01, 0.05), np.arange(-3, 2.51, 0.05)],
                  cmin=1e-9,norm=LogNorm(vmin=0.01),cmap="Blues",
                  weights=np.full((~high).sum(), 1.0 / n_frames))
    ax.scatter(x[high][::20], y[high][::20], s=2, color="tab:purple", alpha=0.3, label="천장 높이 점 (z > 2.1 m)")
    fig.colorbar(h[3], ax=ax, label="점 수 / 프레임 (5 cm 칸)", shrink=0.8)
    draw_cabin_top(ax)
    ax.set_xlim(-2, 2); ax.set_ylim(-3, 2.5)
    ax.set_aspect("equal")
    ax.set_xlabel("x (m, 승강장에서 볼 때 오른쪽 +)")
    ax.set_ylabel("y (m, 캐빈 안쪽 +)")
    ax.set_title("빈 캐빈, 문 닫힘 — 위에서 본 점 분포 (S0-1_t02)")
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)
    print("저장:", out_png)


def plot_sideview(wide_csv, out_png):
    f, t, x, y, z, d = read_points(wide_csv)
    n_frames = len(set(f.tolist()))

    fig, ax = plt.subplots(figsize=(8, 4.5))
    h = ax.hist2d(y, z, bins=[np.arange(-3, 2.51, 0.05), np.arange(-1, 3.01, 0.05)],
                  cmin=1e-9,norm=LogNorm(vmin=0.01),cmap="Blues", weights=np.full(len(y), 1.0 / n_frames))
    fig.colorbar(h[3], ax=ax, label="점 수 / 프레임 (5 cm 칸)")
    ax.add_patch(Rectangle((0, 0), CABIN_D, 2.3, fill=False, lw=1.5, ec="k"))
    ax.axvspan(-DOOR_BAND, 0, color="tab:orange", alpha=0.15, lw=0)
    ax.axhline(0, color="0.4", lw=0.8)
    ax.plot(SENSOR[1], SENSOR[2], marker="*", ms=14, color="red", mec="k")
    ax.annotate("센서", (SENSOR[1], SENSOR[2]), xytext=(6, 6), textcoords="offset points", fontsize=9)
    ax.text(CABIN_D / 2, 0.15, "캐빈 (천장 약 2.3 m 가정)", ha="center", fontsize=8, color="0.3")
    ax.set_xlim(-3, 2.5); ax.set_ylim(-1, 3)
    ax.set_xlabel("y (m, 승강장 −  ←  문턱 0  →  캐빈 안쪽 +)")
    ax.set_ylabel("z (m, 바닥 0)")
    ax.set_title("빈 캐빈, 문 닫힘 — 옆에서 본 점 분포 (S0-1_t02)")
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)
    print("저장:", out_png)


def main():
    p = argparse.ArgumentParser(description="1차 엘리베이터 S0 결과 그림")
    p.add_argument("--door-wide", default="data/output/S0-3_t01_wide.csv")
    p.add_argument("--door-state", default="data/output/S0-3_t01_out_door.csv")
    p.add_argument("--scene-wide", default="data/output/S0-1_t02_wide.csv")
    p.add_argument("--out-dir", default="data/output/figures")
    a = p.parse_args()

    os.makedirs(a.out_dir, exist_ok=True)
    plot_door_state(a.door_wide, a.door_state, os.path.join(a.out_dir, "door_state.png"))
    plot_topview(a.scene_wide, os.path.join(a.out_dir, "topview.png"))
    plot_sideview(a.scene_wide, os.path.join(a.out_dir, "sideview.png"))


if __name__ == "__main__":
    main()
