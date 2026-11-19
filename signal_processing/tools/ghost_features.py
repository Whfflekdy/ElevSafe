"""사람 수를 아는 장면에서 추가 덩어리(유령)의 특징을 사람 덩어리와 비교.
프레임마다 점이 많은 순서로 K개를 사람으로 보고, 나머지를 유령으로 본다.
사용: python tools/ghost_features.py K 파일1_clusters.csv [파일2 ...]

주의
- 사람끼리 붙어 한 덩어리가 되는 장면(S1-5, S1-3)은 쓰지 말 것. 두 번째로 큰 유령이
  사람으로 세어져서 사람 손실이 부풀려진다. 2명 장면은 서로 떨어져 걷는 S1-8_t02만.
- "남는 유령" = 3프레임 이상 같은 자리(0.3 m 안)에 이어서 나온 유령.
  짧은 유령은 C 추적의 3프레임 확정(n_init = 3)이 이미 거르므로, B가 볼 것은 이쪽.

11/19 결론 (4차 S1-8_t01·S1-2·S1-8_t02, 2차 FOV_t03)
- 남는 유령은 뒷벽 띠(cy ≥ 1.30, 유리창·센서 쪽 금속)와 센서 바로 아래에 몰림.
- "뒷벽 띠"는 남는 유령을 48~94% 지우지만, 실제 승객이 뒷벽에 많이 기대 서서 못 씀.
- "뒷벽 띠 + 점 60 미만"은 사람 손실 0~1.4%, 남는 유령 39~56%. 지우기엔 부족, 표시용 후보.
- 뒷벽에 기댄 사람 녹화(5차) 전까지 B는 지우지 않음. 시연은 A의 40점 필터 유지.
"""
import csv, math, sys
from collections import Counter, defaultdict

K = int(sys.argv[1])
SENSOR = (-0.65, 1.31)   # 새 원점 기준 센서 위치 (x, y)
BACK_Y = 1.30            # 뒷벽 띠 시작 (뒷벽은 y +1.49)
FIELDS = ("cx", "cy", "z_top", "z_p50", "num_points", "mean_doppler", "doppler_std",
          "power_sum", "x_min", "x_max", "y_min", "y_max")

# ---------- 1. 읽기: 사람 / 유령 나누기, 유령 지속 시간 ----------
person, ghost = [], []
for path in sys.argv[2:]:
    frames = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if not r.get("cluster_id"):
                continue
            frames[int(r["frame_id"])].append({k: float(r[k]) for k in FIELDS})

    prev_fr, prev_ghosts = None, []
    for fr in sorted(frames):
        cl = sorted(frames[fr], key=lambda c: -c["num_points"])
        top = cl[0]["num_points"]
        cur_ghosts = []
        for i, c in enumerate(cl):
            c["ratio"] = c["num_points"] / top                       # 가장 큰 덩어리 대비 점 수
            c["size"] = max(c["x_max"] - c["x_min"], c["y_max"] - c["y_min"])
            c["pw"] = c["power_sum"] / c["num_points"]               # 점 하나당 평균 power
            c["abs_d"] = abs(c["mean_doppler"])
            if i < K:
                person.append(c)
                continue
            c["d_person"] = min(math.hypot(c["cx"] - p["cx"], c["cy"] - p["cy"]) for p in cl[:K])
            c["age"] = 1                                             # 앞 프레임 0.3 m 안에 유령이 있었으면 이어서 셈
            if prev_fr is not None and fr == prev_fr + 1:
                near = [g for g in prev_ghosts if math.hypot(c["cx"] - g["cx"], c["cy"] - g["cy"]) < 0.3]
                if near:
                    c["age"] = max(g["age"] for g in near) + 1
            cur_ghosts.append(c)
            ghost.append(c)
        prev_fr, prev_ghosts = fr, cur_ghosts

if not person:
    sys.exit("덩어리가 없습니다")

def q(vals, p):
    v = sorted(vals)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float("nan")

# ---------- 2. 특징 비교 ----------
print(f"사람 덩어리 {len(person)}개, 유령 {len(ghost)}개 (사람 {K}명 기준)\n")
print(f"{'특징':<12}{'사람 p10':>9}{'p50':>8}{'p90':>8}   {'유령 p10':>9}{'p50':>8}{'p90':>8}")
for k in ("num_points", "ratio", "size", "z_top", "z_p50", "abs_d", "doppler_std", "pw"):
    a, b = [c[k] for c in person], [c[k] for c in ghost]
    print(f"{k:<12}{q(a,.1):9.2f}{q(a,.5):8.2f}{q(a,.9):8.2f}   {q(b,.1):9.2f}{q(b,.5):8.2f}{q(b,.9):8.2f}")
for k in ("d_person", "age"):
    b = [c[k] for c in ghost]
    print(f"{k:<12}{'':>25}   {q(b,.1):9.2f}{q(b,.5):8.2f}{q(b,.9):8.2f}")

# ---------- 3. 유령이 자주 생기는 자리 ----------
def cell(c):
    return (round(c["cx"] / 0.2) * 0.2, round(c["cy"] / 0.2) * 0.2)

print("\n유령이 자주 생기는 자리 (0.2 m 칸, 상위 6)")
for (x, y), n in Counter(cell(c) for c in ghost).most_common(6):
    ages = [c["age"] for c in ghost if cell(c) == (x, y)]
    print(f"  ({x:+.1f}, {y:+.2f})  {n:4d}개  최대 지속 {max(ages)}프레임")

# ---------- 4. 규칙 후보 ----------
def near_sensor(c, r=0.4):
    return math.hypot(c["cx"] - SENSOR[0], c["cy"] - SENSOR[1]) < r

def back(c):
    return c["cy"] >= BACK_Y

print(f"\n사람이 뒷벽 띠(cy ≥ {BACK_Y})에 있던 덩어리: {sum(back(c) for c in person)}개")
print(f"사람이 센서 0.4 m 안에 있던 덩어리: {sum(near_sensor(c) for c in person)}개")

persist = [c for c in ghost if c["age"] >= 3]
print(f"\n규칙 후보: 사람 손실 / 유령 제거 / 남는 유령 제거 (남는 유령 {len(persist)}개)")
rules = {
    "점 40 미만 (지금 A 필터)":          lambda c: c["num_points"] < 40,
    "가장 큰 덩어리의 30% 미만":         lambda c: c["ratio"] < 0.3,
    "뒷벽 띠":                           back,
    "뒷벽 띠 + 점 60 미만":              lambda c: back(c) and c["num_points"] < 60,
    "(센서 or 뒷벽 띠) + 점 60 미만":    lambda c: (near_sensor(c) or back(c)) and c["num_points"] < 60,
}
def pct(f, xs):
    return sum(f(c) for c in xs) / len(xs) * 100 if xs else 0.0

for name, f in rules.items():
    print(f"  {name:<28} 사람 {pct(f, person):5.1f}%  유령 {pct(f, ghost):5.1f}%  남는 유령 {pct(f, persist):5.1f}%")