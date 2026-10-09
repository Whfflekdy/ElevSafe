"""
pcr_targets_to_csv.py
PCR 의 센서 Detection(targets) 을 CSV 로 저장한다. (점 데이터는 pcr_to_csv.py 사용)

사용법:
  python3 tools/pcr_targets_to_csv.py <in.pcr> <out.csv> [--session N]
  sg-radar-tools 위치가 다르면 환경변수 SG_RADAR_TOOLS 로 지정
"""
import argparse
import csv
import os
import sys
from pathlib import Path

TOOLS = os.environ.get("SG_RADAR_TOOLS", str(Path(__file__).resolve().parents[2] / ".." / "sg-radar-tools"))
sys.path.insert(0, str(Path(TOOLS) / "tools" / "pcr"))
from pcr_loader import load                                         # noqa: E402
from pcr_to_csv import select_session, iter_frames_with_elapsed_us  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("pcr")
ap.add_argument("out")
ap.add_argument("--session", type=int, default=None)
args = ap.parse_args()

idx, session = select_session(load(args.pcr), args.session)
rows = 0
with open(args.out, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["frame", "timestamp_us", "target_id", "x", "y", "status",
                "x_min", "x_max", "y_min", "y_max", "z_min", "z_max"])
    for frame, elapsed_us in iter_frames_with_elapsed_us(session.frames):
      for t in frame.targets:
        has_box = t.minx <= t.maxx          # 점이 없으면 min > max 로 남아 있음
        box = [f"{v:.3f}" if has_box else "" for v in
               (t.minx, t.maxx, t.miny, t.maxy, t.minz, t.maxz)]
        w.writerow([frame.frame_count, elapsed_us, t.target_id,
                    f"{t.x:.3f}", f"{t.y:.3f}", t.status, *box])
        rows += 1
print(f"session index={idx}, id={session.id}, name='{session.name}' → Detection {rows}개 → {args.out}")