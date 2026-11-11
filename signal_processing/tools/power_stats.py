#!/usr/bin/env python3
"""덩어리 점과 노이즈 점의 세기 분포를 비교한다 (Step 3 임계값 설계용).

사용법: python3 tools/power_stats.py <설정.cfg> <출력.csv> [<출력.csv> ...]
<출력.csv> 는 radar_pipeline 의 점 단위 결과. 센서 위치는 설정 파일에서 읽는다.

power = 수신 세기 (선형 값). 거리가 멀수록 약해지므로 거리 효과를 보정해 비교한다.
  power_db = 10 log10(power)
  rcs_db   = power_db + 40 log10(거리)   상대값이라 단위 없이 서로 비교만 한다
"""
import csv
import math
import sys


def sensor_position(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0]
            if "=" in line:
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
    return tuple(float(values.get(k, 0)) for k in ("sensor.tx", "sensor.ty", "sensor.tz"))


def pct(values, p):
    return values[min(len(values) - 1, int(len(values) * p / 100))]


def line(name, values):
    if not values:
        print(f"  {name:<8} 없음")
        return
    values.sort()
    print(f"  {name:<8} {len(values):>7}점  최저 {values[0]:6.1f}  5% {pct(values, 5):6.1f}  "
          f"중앙 {pct(values, 50):6.1f}  95% {pct(values, 95):6.1f}  최고 {values[-1]:6.1f}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    tx, ty, tz = sensor_position(sys.argv[1])
    for path in sys.argv[2:]:
        groups = {"덩어리": [], "노이즈": []}
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                power = float(row["power"])
                if power <= 0:
                    continue
                r = math.dist((float(row["x"]), float(row["y"]), float(row["z"])), (tx, ty, tz))
                if r <= 0:
                    continue
                rcs = 10 * math.log10(power) + 40 * math.log10(r)
                groups["덩어리" if int(row["cluster"]) >= 0 else "노이즈"].append(rcs)
        print(path)
        line("덩어리", groups["덩어리"])
        line("노이즈", groups["노이즈"])


if __name__ == "__main__":
    main()
