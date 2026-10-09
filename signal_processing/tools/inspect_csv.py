"""
inspect_csv.py
CSV(입력 또는 radar_pipeline 출력)의 점 분포를 요약한다.

사용법:
  python3 tools/inspect_csv.py <a.csv> [b.csv]
      y 거리 구간별 점 개수. 두 파일을 주면 나란히 비교 (예: 배경 제거 전/후)

  python3 tools/inspect_csv.py --clusters <output.csv>
      radar_pipeline 출력에서 덩어리별 중심점 (리허설 좌표 확인용)
"""
import csv
import sys
from collections import Counter, defaultdict


def summary(path):
    rows = list(csv.DictReader(open(path)))
    frames = len({r["frame"] for r in rows})
    ys = [float(r["y"]) for r in rows]
    print(f"[{path}] 프레임 {frames}개, 점 {len(rows)}개 "
          f"(프레임당 {len(rows) / max(frames, 1):.0f}개)")
    return Counter(int(y // 0.5) for y in ys)


def histogram(paths):
    hists = [summary(p) for p in paths]
    keys = sorted(set().union(*hists))
    print("\n y 거리 구간      " + "".join(f"{'파일' + str(i + 1):>10}" for i in range(len(hists))))
    for k in keys:
        counts = "".join(f"{h.get(k, 0):>10}" for h in hists)
        print(f"  {k * 0.5:4.1f} ~ {k * 0.5 + 0.5:4.1f} m  {counts}")


def clusters(path):
    rows = [r for r in csv.DictReader(open(path)) if r.get("cluster", "-1") != "-1"]
    if not rows:
        print(f"[{path}] 덩어리가 없습니다 (cluster 컬럼이 없거나 전부 노이즈)")
        return
    by_frame = defaultdict(lambda: defaultdict(list))
    for r in rows:
        by_frame[r["frame"]][r["cluster"]].append(r)

    print(f"[{path}] 덩어리 있는 프레임 {len(by_frame)}개")
    frames = list(by_frame)
    for f in frames[:: max(1, len(frames) // 5)]:          # 고르게 5개 프레임만 표시
        print(f"  frame {f}")
        for c, pts in sorted(by_frame[f].items(), key=lambda kv: -len(kv[1])):
            n = len(pts)
            mx, my, mz = (sum(float(p[k]) for p in pts) / n for k in ("x", "y", "z"))
            print(f"    #{c}: 점 {n:4d}개  x={mx:+.2f}  y={my:+.2f}  z={mz:.2f}  zone={pts[0]['zone']}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--clusters":
        clusters(args[1])
    elif 1 <= len(args) <= 2:
        histogram(args)
    else:
        sys.exit(__doc__)