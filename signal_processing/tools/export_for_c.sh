#!/bin/bash
# export_for_c.sh
# 한 회차의 녹화 CSV를 모두 처리해 C에게 넘길 결과와 요약표를 만든다.
#
# 사용법 (signal_processing 폴더에서):
#   bash tools/export_for_c.sh <설정.cfg> <입력 폴더> <출력 폴더>
# 예:
#   bash tools/export_for_c.sh config/elevator_2nd.cfg data/raw/elevator2 data/output/to_C/20261107_recording_1
#
# 출력 폴더에 테이크마다 3개 파일(<이름>.csv, _door.csv, _clusters.csv)과
# summary.csv(테이크별 프레임 수, 덩어리 수, 문 상태)를 만든다. 실행 로그는 logs/ 아래.

CFG="$1"
IN_DIR="$2"
OUT_DIR="$3"
PIPELINE="./build/radar_pipeline"

if [ $# -ne 3 ]; then
  sed -n '3,12p' "$0"
  exit 1
fi
if [ ! -x "$PIPELINE" ] || [ ! -f "$CFG" ] || [ ! -d "$IN_DIR" ]; then
  echo "[중단] 실행 파일, 설정, 입력 폴더를 확인하세요 (signal_processing 폴더에서 실행)"
  exit 1
fi
mkdir -p "$OUT_DIR/logs"

SUMMARY="$OUT_DIR/summary.csv"
echo "take,frames,clusters_per_frame,open_frames,door_transitions,noise_points" > "$SUMMARY"

count=0
for input in "$IN_DIR"/*.csv; do
  name=$(basename "$input" .csv)
  log="$OUT_DIR/logs/$name.log"
  if ! "$PIPELINE" "$CFG" "$input" "$OUT_DIR/$name.csv" > "$log" 2>&1; then
    echo "[실패] $name (로그: $log)"
    echo "$name,실패,,,," >> "$SUMMARY"
    continue
  fi
  frames=$(sed -n 's/.*결과 (\([0-9]*\) frames).*/\1/p' "$log")
  per=$(sed -n 's/.*프레임당 평균 덩어리 \([0-9.e-]*\)개.*/\1/p' "$log")
  noise=$(sed -n 's/.*노이즈 점 \([0-9]*\)개.*/\1/p' "$log")
  open=$(sed -n 's/.*OPEN 프레임 \([0-9]*\) \/.*/\1/p' "$log")
  trans=$(sed -n 's/.*상태 전환 \([0-9]*\)번.*/\1/p' "$log")
  echo "$name,$frames,$per,$open,$trans,$noise" >> "$SUMMARY"
  count=$((count + 1))
done

echo "처리한 테이크 ${count}개 → $OUT_DIR"
column -s, -t "$SUMMARY" 2>/dev/null || cat "$SUMMARY"
