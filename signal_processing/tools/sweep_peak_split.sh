#!/bin/bash
# sweep_peak_split.sh
# 봉우리 분할 값을 바꿔 가며 리허설 장면의 덩어리 수를 비교한다.
#
# 사용법 (signal_processing 폴더에서):
#   bash tools/sweep_peak_split.sh
#
# config/rehearsal.cfg 는 건드리지 않고, 같은 폴더에 임시 설정을 만들어 쓴 뒤 지운다.
# 같은 키가 두 번 있으면 뒤의 값이 쓰이는 Config::load 동작을 이용한다.

PIPELINE="./build/radar_pipeline"
BASE_CFG="config/rehearsal.cfg"
TMP_CFG="config/_sweep_tmp.cfg"
RAW="data/raw/rehearsal"
OUT="data/output/sweep"
ONE_PERSON="R1-1 R1-2 R1-3 R2-1"     # 늘어나면 안 되는 1명 장면
TWO_PEOPLE="R3-1"                    # 늘어나야 하는 2명 장면

# 조합: "min_valley min_distance min_height"
COMBOS=(
  "0.15 0.30 0.9"
  "0.10 0.30 0.9"
  "0.05 0.30 0.9"
  "0.15 0.25 0.9"
  "0.10 0.25 0.9"
  "0.05 0.25 0.9"
  "0.10 0.20 0.9"
)

if [ ! -x "$PIPELINE" ] || [ ! -f "$BASE_CFG" ]; then
  echo "signal_processing 폴더에서 빌드 후 실행하세요"
  exit 1
fi
mkdir -p "$OUT"
trap 'rm -f "$TMP_CFG"' EXIT

# 장면 하나 실행 → "덩어리/프레임(나눈 수)" 출력
run_scene() {
  local log
  log=$("$PIPELINE" "$TMP_CFG" "$RAW/$1_t01.csv" "$OUT/$1.csv" 2>&1)
  local count split
  count=$(printf '%s\n' "$log" | sed -n 's/.*프레임당 평균 덩어리 \([0-9.]*\)개.*/\1/p')
  split=$(printf '%s\n' "$log" | sed -n 's/.*나눈 덩어리 \([0-9]*\)개.*/\1/p')
  printf '%.3f(%s)' "${count:-0}" "${split:-?}"
}

printf '%-7s %-6s %-6s' valley dist height
for s in $TWO_PEOPLE $ONE_PERSON; do printf ' %-13s' "$s"; done
echo

for combo in "${COMBOS[@]}"; do
  read -r valley dist height <<< "$combo"
  {
    cat "$BASE_CFG"
    echo
    echo "peak_split.enabled = 1"
    echo "peak_split.min_valley = $valley"
    echo "peak_split.min_distance = $dist"
    echo "peak_split.min_height = $height"
  } > "$TMP_CFG"

  printf '%-7s %-6s %-6s' "$valley" "$dist" "$height"
  for s in $TWO_PEOPLE $ONE_PERSON; do printf ' %-13s' "$(run_scene "$s")"; done
  echo
done
