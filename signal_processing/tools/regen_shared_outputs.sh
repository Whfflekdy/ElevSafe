#!/bin/bash
# regen_shared_outputs.sh
# 드라이브에 공유한 출력 CSV를 현재 코드·설정으로 다시 만들고, 예전 파일과 비교해 요약한다.
#
# 사용법 (signal_processing 폴더에서):
#   bash tools/regen_shared_outputs.sh
#
# 하는 일 (파일마다):
#   1. 예전(ms) 출력이 있으면 <이름>_out_ms_old.csv 로 옮겨 둔다
#      (헤더가 timestamp_ms 일 때만 옮기므로, 다시 돌려도 새 파일이 "예전"으로 밀리지 않음)
#   2. radar_pipeline 실행, 로그는 data/output/regen_logs/<이름>.log
#   3. 배경 칸 수가 기대값과 같은지 확인 (다르면 엉뚱한 배경 맵을 쓴 것)
#   4. 예전 파일과 비교: 지워진 점 / 값이 바뀐 점 / cluster 번호가 바뀐 점

# ── 작업 목록: 이름 | 설정 | 입력 CSV | 기대 배경 칸 수 ──────────────────
# 입력 경로가 다르면 여기만 고치면 된다.
JOBS=(
  "front_man|config/minitest.cfg|data/raw/minitest/front_man.csv|910"
  "conn_bg|config/minitest.cfg|data/raw/minitest/conn_bg.csv|910"
  "home_empty02|config/minitest_home.cfg|data/raw/home/empty_home_02.csv|3189"
  "home_chair|config/minitest_home.cfg|data/raw/home/1p_chair_home_01.csv|3189"
  "home_stand|config/minitest_home.cfg|data/raw/home/1p_stand_home_01.csv|3189"
)

OUT_DIR="data/output"
LOG_DIR="$OUT_DIR/regen_logs"
PIPELINE="./build/radar_pipeline"

# ── 시작 전 확인 ─────────────────────────────────────────────────────────
if [ ! -x "$PIPELINE" ]; then
  echo "[중단] $PIPELINE 이 없음. signal_processing 폴더에서 실행했는지, 빌드했는지 확인"
  exit 1
fi
mkdir -p "$LOG_DIR"

# 파일 안에서 이 정규식에 맞는 줄의 마지막 숫자를 꺼낸다 (없으면 '-')
last_number() {   # $1 = 패턴, $2 = 파일
  local v
  v=$(grep -E "$1" "$2" | head -1 | grep -oE '[0-9]+' | tail -1)
  echo "${v:--}"
}

# 두 파일의 diff 결과 중 '<' 또는 '>' 로 시작하는 줄 수
count_diff() {    # $1 = '<' 또는 '>', $2 = cut 범위, $3 = 예전, $4 = 새
  diff <(cut -d, -f"$2" "$3") <(cut -d, -f"$2" "$4") | grep -c "^$1" || true
}

SUMMARY=""
WARN=0

for job in "${JOBS[@]}"; do
  IFS='|' read -r name cfg input expect <<< "$job"
  out="$OUT_DIR/${name}_out.csv"
  old="$OUT_DIR/${name}_out_ms_old.csv"
  log="$LOG_DIR/${name}.log"

  echo "── $name ──"

  if [ ! -f "$input" ]; then
    echo "  [건너뜀] 입력 없음: $input  (JOBS 의 경로를 고쳐주세요)"
    SUMMARY+="$name|입력 없음|-|-|-|-|-|-"$'\n'
    WARN=1
    continue
  fi
  if [ ! -f "$cfg" ]; then
    echo "  [건너뜀] 설정 없음: $cfg"
    SUMMARY+="$name|설정 없음|-|-|-|-|-|-"$'\n'
    WARN=1
    continue
  fi

  # 1. 예전 출력 보관: ms 형식 파일만, 그리고 이미 보관본이 없을 때만
  if [ -f "$out" ] && [ ! -f "$old" ] && head -1 "$out" | grep -q 'timestamp_ms'; then
    mv "$out" "$old"
    echo "  예전 출력 보관: $old"
  fi

  # 2. 실행
  if ! "$PIPELINE" "$cfg" "$input" "$out" > "$log" 2>&1; then
    echo "  [실패] radar_pipeline 오류, 로그: $log"
    SUMMARY+="$name|실행 실패|-|-|-|-|-|-"$'\n'
    WARN=1
    continue
  fi

  # 3. 결과 요약 꺼내기
  cells=$(last_number '배경 칸 [0-9]+개' "$log")
  points=$(last_number '^입력 포인트' "$log")
  removed_bg=$(last_number '^배경 제거' "$log")

  if [ "$cells" != "$expect" ]; then
    echo "  [경고] 배경 칸 $cells 개 (기대 $expect) → 설정의 map_path 확인"
    WARN=1
  fi

  # 출력 헤더가 µs 인지
  header=$(head -1 "$out")
  case "$header" in
    *timestamp_us*) ;;
    *) echo "  [경고] 출력 헤더에 timestamp_us 없음: $header"; WARN=1 ;;
  esac

  # 4. 예전 파일과 비교
  #    f1,3-9  = frame + 점 값(x..zone)   → 지워진 점(<), 값이 바뀐 점(>)
  #    f1,3-   = 위 + cluster            → cluster 까지 포함해 바뀐 점(>)
  if [ -f "$old" ]; then
    gone=$(count_diff '<' '1,3-9' "$old" "$out")
    changed=$(count_diff '>' '1,3-9' "$old" "$out")
    relabel=$(count_diff '>' '1,3-' "$old" "$out")
    if [ "$changed" != "0" ]; then
      echo "  [경고] 값이 바뀐 점 $changed 개 → 결과 확인 필요"
      WARN=1
    fi
  else
    gone="-"; changed="-"; relabel="-"
    echo "  (예전 파일 없음, 비교 생략)"
  fi

  echo "  배경 칸 $cells / 입력 $points / 배경 제거 $removed_bg / 지워짐 $gone / 값 바뀜 $changed / cluster 포함 바뀜 $relabel"
  SUMMARY+="$name|$cells|$expect|$points|$removed_bg|$gone|$changed|$relabel"$'\n'
done

# ── 요약 표 ──────────────────────────────────────────────────────────────
echo
echo "=== 요약 ==="
echo "cells=배경 칸, bg_rm=배경 제거 점, gone=예전보다 사라진 점, changed=값이 바뀐 점(0이어야 함), relabel=cluster 포함 바뀐 점"
printf "%-14s %8s %6s %9s %9s %8s %7s %9s\n" \
  "name" "cells" "expect" "points" "bg_rm" "gone" "changed" "relabel"
printf "%s" "$SUMMARY" | while IFS='|' read -r n c e p r g ch rl; do
  [ -z "$n" ] && continue
  printf "%-14s %8s %6s %9s %9s %8s %7s %9s\n" "$n" "$c" "$e" "$p" "$r" "$g" "$ch" "$rl"
done
echo
if [ "$WARN" = "0" ]; then
  echo "경고 없음. 드라이브에 *_out.csv 를 교체하면 됩니다."
else
  echo "경고가 있습니다. 위 메시지와 $LOG_DIR 의 로그를 확인하세요."
fi
