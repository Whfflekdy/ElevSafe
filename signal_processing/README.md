# signal_processing — 신호 처리 및 클러스터링

4D 레이더(Retina-4SN) 포인트 클라우드에서 엘리베이터 출입구(Door Zone)와 내부(Cabin)의 유효 포인트를 정제하고,
클러스터링을 통해 탑승객 단위의 객체를 추출하는 모듈입니다. 레이더로 문 열림·닫힘도 추정합니다.

담당: 김수연 (B)

## 파이프라인

```
FrameData → 격자 번호 → 좌표 변환 → 문 상태 추정 → ROI 필터 → 정적 배경 제거 → SOR/RCS → DBSCAN → 덩어리 요약 → PipelineResult
            (Step 2)   (Step 1)    (문 상태)      (Step 1)    (Step 2)        (Step 3)  (Step 4)   (Step 5)
```

- [x] **Step 1**: 센서 → 엘리베이터 좌표 변환, Door Zone / Cabin ROI 필터 및 Zone 라벨링
  - [x] 1차 엘리베이터 실측 설정 (`config/elevator_1st.cfg`)
  - [ ] 위치별 좌표 검증 (FOV 재촬영 후)
- [x] **Step 2**: 극좌표(u-v) 격자 기반 정적 배경 제거 (배경 맵 생성·저장·적용)
  - [x] 실제 빈 장면 + 정지 승객 데이터로 검증 (집 녹화: 정지 사람 제거 0, 다른 빈 장면 87.7% 제거)
  - [x] Power 보조 판단 → 불필요 (배경인 의자 옆 사람도 지워지지 않음)
  - [x] occupancy_threshold / dilation 튜닝 → 기준 20%, dilation 1
  - [x] 1차 엘리베이터: 빈 캐빈은 ROI만으로 거의 깨끗함 (아래 "1차 엘리베이터 결과")
  - [~] 문 닫힘·열림 공통 칸 맵 → 보류 (엘리베이터에서는 문·승강장이 ROI 밖이라 효과 없음)
- [x] **문 상태 추정**: 문 너머가 보이는지로 OPEN / CLOSED 판단 (1차 엘리베이터 검증)
- [ ] **Step 3**: 허공 노이즈 제거 (RCS, SOR)
- [ ] **Step 4**: DBSCAN 클러스터링 및 파라미터 튜닝
  - [x] DBSCAN 구현 (공통 틀 `Clusterer`로 알고리즘 교체 가능), 파이프라인 연결, 출력에 `cluster` 컬럼
  - [x] 사람 1명 데이터로 출발 파라미터 결정
  - [x] 두 사람 나란히 확인 (리허설 R3-1): 약 절반의 프레임에서 한 덩어리로 합쳐짐
  - [ ] 변형 알고리즘 (격자 칸 기준 이웃 등, [docs/dbscan_design.md](docs/dbscan_design.md))
- [ ] **Step 5**: 덩어리 요약 및 대형 군집 분할
  - [x] 1차: 덩어리마다 중심·범위·`z_top`(상위 95%)·평균 도플러·점 수·zone
  - [ ] 봉우리 분할 + `maybe_merged` (지금은 항상 false)
  - [ ] 사람이 아닌 덩어리 거르기 (문짝 위쪽 끝, 작은 물체)
- [x] **A 연동 진입점**: `SignalPipeline` 클래스, `elevsafe::sp` 이름 공간, C++20 빌드 확인

## 폴더 구조

```
signal_processing/
├── CMakeLists.txt
├── config/
│   ├── elevator_default.cfg       # 엘리베이터 설정 틀
│   ├── elevator_1st.cfg           # 1차 엘리베이터 녹화 (실측)
│   ├── elevator_1st_wide.cfg      # 같은 센서 값, ROI 해제·배경 제거 끔 (장면 전체 확인용)
│   ├── rehearsal.cfg              # 강의실 리허설 (실측)
│   ├── rehearsal_floor.cfg        # 리허설 바닥 높이 확인용
│   ├── rehearsal_wide.cfg         # 리허설 장면 전체 확인용
│   ├── minitest.cfg               # 엘리베이터가 아닌 장소(책상 위 센서) 데이터 테스트용
│   └── minitest_home.cfg          # 집 녹화 (배경 맵 empty_home_01)
├── include/
│   ├── common/                    # radar_point.h, box3d.h, config.h, door_state.h
│   ├── io/                        # csv_io.h
│   ├── preprocessing/             # coordinate_transform.h, roi_filter.h, polar_grid.h,
│   │                              # background_subtractor.h, door_state_estimator.h
│   ├── clustering/                # clusterer.h (공통 틀), dbscan.h
│   └── pipeline/                  # signal_pipeline.h (공개 진입점, A가 include)
├── src/                           # 구현 (include와 같은 구조)
│   ├── main.cpp                   # radar_pipeline: 오프라인 실행기 (SignalPipeline 사용)
│   └── build_background.cpp       # build_background: 빈 장면 → 배경 맵 생성
├── docs/
│   └── dbscan_design.md           # DBSCAN 변형 설계 (구상)
├── tools/                         # inspect_csv.py, pcr_targets_to_csv.py, regen_shared_outputs.sh
├── tests/
└── data/
    ├── raw/                       # 실측 녹화 CSV (git 제외)
    ├── sample/                    # 테스트용 소형 데이터 (tiny.csv)
    ├── background/                # 생성된 배경 맵 (git 제외)
    └── output/                    # 실행 결과 (git 제외)
```

## 빌드 및 실행

```bash
cd signal_processing
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # 처음 한 번, 또는 CMakeLists.txt 수정 시
cmake --build build
```

빌드 종류를 정하지 않으면 최적화 없이 컴파일되어 처리 시간이 여러 배 느려집니다.
`grep CMAKE_BUILD_TYPE build/CMakeCache.txt`가 `Release`인지 확인하세요.

**파이프라인 실행**

```bash
./build/radar_pipeline [config] [input.csv] [output.csv]
```

인자를 생략하면 `config/elevator_default.cfg`, `data/sample/tiny.csv`, `data/output/roi_out.csv`를 사용합니다.
설정 파일의 `background.map_path`에 배경 맵이 없으면 배경 제거만 건너뛰고 나머지는 그대로 실행합니다.

출력 파일은 세 개입니다 (형식은 아래 "데이터 형식").

| 파일 | 내용 |
|---|---|
| `output.csv` | 점 단위 결과 (zone, cluster 포함) |
| `output_door.csv` | 프레임별 문 상태 |
| `output_clusters.csv` | 덩어리 요약 (B → C 형식) |

**배경 맵 생성**

```bash
mkdir -p data/background   # 처음 한 번 (배경 맵은 git에 올리지 않아 폴더가 없을 수 있음)
./build/build_background [config] [empty.csv] [map.csv]
```

빈 장면 CSV로 배경 맵을 만들어 저장합니다. 이후 `radar_pipeline`이 설정 파일의 `background.map_path`에서 불러옵니다.
배경 맵은 센서 기준 격자로 만들기 때문에, 설정 파일의 센서 실측값을 고쳐도 다시 만들 필요가 없습니다.

**녹화 데이터(PCR) 사용 순서**

1. [sg-radar-tools](https://github.com/jin-1031/sg-radar-tools)의 `tools/pcr/pcr_to_csv.py`로 PCR → CSV 변환
   (Python 가상환경에서 `pip install zstandard` 필요, 세션이 여러 개면 `--session N`)
2. 변환한 CSV를 `data/raw/` 아래에 저장
3. 빈 장면 CSV로 `build_background` 실행 → 대상 CSV로 `radar_pipeline` 실행

PCR에 세션이 여러 개면(녹화 후 세션 삭제를 빠뜨린 경우), 앞 장면과 프레임 수가 같은 세션은 빼고 녹화 길이(초 × 약 20)와 맞는 번호를 고릅니다.

## Backend 연동 (A)

공개 헤더는 `include/pipeline/signal_pipeline.h` 하나이며, 모든 타입은 `elevsafe::sp` 이름 공간 안에 있습니다.

```cmake
add_subdirectory(<ElevSafe>/signal_processing sp)
target_link_libraries(<backend_target> PRIVATE radar_core)
```

```cpp
#include "pipeline/signal_pipeline.h"

elevsafe::sp::Config cfg;
cfg.load("<signal_processing>/config/elevator_1st.cfg");
elevsafe::sp::SignalPipeline signal(cfg);       // 시작할 때 한 번, signal.ok() 확인
auto r = signal.process(fd, ctx);                // 프레임마다
```

- `fd`: A의 Adapter가 `RadarFrame`을 바꾼 `FrameData` (센서 원본 좌표 그대로, `timestamp_us`는 int64)
- `ctx`: `FrameContext` (`door_state`, 모르면 기본값 UNKNOWN)
- `r`: `frame_id`, `timestamp_us`, `clusters`(C에 전달, 없으면 빈 목록), `estimated_door_state`
- 문 상태 추정이 최근 프레임을 기억하므로 **센서 하나에 객체 하나**, 같은 객체를 여러 스레드에서 동시에 부르지 않음
- `add_subdirectory`로 가져가면 `radar_core`만 빌드되고, 도구 실행 파일은 단독 빌드일 때만 만들어짐
- 바깥 프로젝트(C++20)에서 S0-3를 처리해 오프라인과 같은 결과(덩어리 합 333, 문 전환 7번)를 확인

자세한 형식은 A-B · B-C 인터페이스 합의 문서를 참고하세요.

## 좌표계

모든 ROI와 이후 처리는 **엘리베이터 좌표계** 기준입니다.

- 원점: 캐빈 문턱(sill) 중앙의 바닥면
- x: 좌우 (**승강장에서 캐빈을 바라볼 때** 오른쪽 +)
- y: 깊이 (캐빈 안쪽 +, 승강장 쪽 −)
- z: 높이 (바닥 0, 위쪽 +)
- 오른손 좌표계, 단위 m

변환식: `p_엘리베이터 = R × p_센서 + t`
(R = Rz(yaw) · Ry(pitch) · Rx(roll), t = 엘리베이터 원점 기준 센서 설치 위치)

> **센서 축 (Retina-4SN, 2026-10-05 미니테스트로 확인)**
> 정면 +Y, 오른쪽 +X, 위 +Z, 오른손 좌표계 / 단위 m / Doppler: 접근 −, 이탈 +
> - 아래로 숙여 설치하면 `roll_deg` 음수 (예: 25° 하방 → `-25`)
> - 뒷벽에서 문을 향해 설치하면 `yaw_deg = 180`
> - 검증: `front_man`(정면 1m) 사람 점 평균 (x −0.02, y 1.25), `right_man`(오른쪽 1m) 사람 점 x 0.75 ~ 1.25

> **각도 이름 주의**: 코드는 축 기준 이름이라 `roll_deg`가 **아래로 숙임**(센서 x축 회전), `pitch_deg`가 **좌우 기울기**입니다.
> `yaw_deg`는 위에서 내려다본 방향으로, 0 = 캐빈 안쪽, 90 = 승강장에서 볼 때 왼쪽, **180 = 문 쪽**, 270 = 오른쪽입니다.
> 뒤쪽 모서리에서 원점을 향해 달면 `yaw = 180 + atan(−tx / ty)` (x<0 모서리 ≈ 207, x>0 모서리 ≈ 153).

## 센서 격자 구조 (실측)

Retina-4SN의 포인트는 연속 공간이 아니라 **거리와 방향 코사인(u-v)의 격자** 위에만 찍힙니다.
미니테스트 데이터(`front_man`, `conn_bg`)로 측정했으며, 모든 점이 오차 0칸으로 격자 위에 있었습니다.

| 항목 | 정의 | 간격 | 기준점 |
|---|---|---|---|
| 거리 r | √(x² + y² + z²) | **0.071565 m** | 0 |
| 좌우 u | x / r (방위 방향 코사인) | **0.015938** (정면 근처 약 0.91°) | 0 |
| 위아래 v | z / r = sin(고도각) | **0.015938** | 0 |

- 칸 번호 = `round(값 / 간격)`. 각도(atan, asin)가 아니라 u, v가 일정 간격이라는 점에 주의
- u 칸 번호 범위 약 −47 ~ 47 → 좌우 시야각 약 ±49°
- 작년 레포트의 수치(거리 75mm, 각도 1.04° / 1.27°, 각도 격자)와 다르므로 이 값을 기준으로 함
- 격자 번호는 **센서 좌표 기준**이라 좌표 변환 전에 계산해 `RadarPoint`에 저장 (`has_bins`, `range_bin`, `u_bin`, `v_bin`)
- 거리 칸 0.071565 m는 거리 해상도 c / (2 × 대역폭) 기준 약 2.1 GHz 대역폭에 해당

## 정적 배경 제거

빈 장면에서 **전체 프레임 중 일정 비율 이상 점이 찍힌 격자 칸**을 배경으로 등록하고, 이후 그 칸에 떨어진 점을 제거합니다.
도플러 기반 제거는 정지한 승객까지 지우므로 사용하지 않습니다.
같은 이유로 맵을 만들 때 움직이는 점을 빼지도 않습니다 (계속 도는 환풍기 같은 물체가 맵에서 빠져 유령 덩어리가 됨).

| 설정 키 | 기본값 | 설명 |
|---|---|---|
| `background.map_path` | — | 배경 맵 파일 경로 (없으면 배경 제거 생략, 상대 경로는 설정 파일 위치 기준) |
| `background.occupancy_threshold` | 0.2 | 이 비율 이상 프레임에서 찍힌 칸을 배경으로 등록 |
| `background.dilation` | 1 | 배경 칸 주변 몇 칸까지 배경으로 넓힐지 (센서 진동 대비) |

배경 맵 파일은 `range_bin,u_bin,v_bin` 형식의 CSV입니다.

### 배경 맵 사용 원칙 (미니테스트·리허설 실험 결과)

| 실험 | 결과 | 교훈 |
|---|---|---|
| `conn_bg`로 맵 생성 → `conn_bg`에 적용 | 94.9% 제거 (이전 기본값 72.9%) | 코드 동작 확인 |
| `conn_bg`로 맵 생성 → `front_man`에 적용 | **0% 제거**, 겹치는 칸 0개 | 센서 배치가 다르면 배경 맵을 전혀 쓸 수 없음 |
| `doppler_man`(사람 포함)으로 맵 생성 → 자기 자신에 적용 | 사람이 멈춰 선 3m 근처만 제거 | 사람이 섞인 데이터로 만들면 **정지한 승객이 지워짐** |
| 리허설 R0-1(중간에 사람이 센서 근처를 지나감) | 자기 자신에게도 22.8%만 맞음 | 빈 장면 녹화 중 사람이 움직이면 맵이 부실해짐 |

따라서 배경 맵은 반드시

1. **사람이 전혀 없는 빈 장면**으로,
2. **실제 측정과 같은 센서 설치 상태**에서 (센서 설치 직후, 승객 시나리오 전에) 만들어야 하며,
3. 센서를 다시 설치하거나 각도를 바꾸면 **다시 생성**해야 합니다.

### 1차 엘리베이터 결과 (S0, 빈 캐빈)

- **빈 캐빈은 ROI만으로 거의 깨끗함**: 닫힘 S0-1_t02에서 ROI 통과 0.25점/프레임, 덩어리 0.012개/프레임.
  벽·문·천장·승강장이 모두 ROI 밖이라 배경 맵이 지울 정지 물체가 ROI 안에 남지 않음 (배경 제거 0%)
- **빈 캐빈 점의 약 80%는 천장 회전체**: 문 위 앞쪽 오른쪽 천장(z 2.2 ~ 2.4 m)에서 프레임당 약 48점, 양방향 도플러, 3분 내내 일정 → 환풍기로 추정. ROI 높이로 제외되고 배경 맵에도 등록됨
- **닫힌 금속 문은 거의 찍히지 않음**: 모서리에서 비스듬히 보면 매끈한 금속면이 전파를 다른 방향으로 반사. 캐빈 바닥도 마찬가지로 점이 적음
- **배경 맵의 역할**: ROI 안에 정지 물체(손잡이, 두고 간 물건 등)가 있을 때의 안전장치. 1차 녹화의 파워뱅크처럼 점이 듬성한 물체는 20% 기준에 못 미쳐 등록되지 않음

## 문 상태 추정

닫힌 금속 문은 전파를 막고 자기 자신은 잘 찍히지 않으므로, **"문이 보이는가"가 아니라 "문 너머 승강장이 보이는가"**로 판단합니다.
좌표 변환 직후, ROI가 문 너머 점을 지우기 전에 계산합니다.

- 문 너머 상자 안의 점 수 → 최근 `window` 프레임 평균
- 평균 ≥ `open_threshold`면 OPEN, ≤ `close_threshold`면 CLOSED, 그 사이는 이전 상태 유지 (히스테리시스)
- 처음 `window` 프레임은 UNKNOWN

| 설정 키 | 기본값 | 설명 |
|---|---|---|
| `door_state.view.x_min/x_max` | −0.44 / 0.44 | 문 폭 |
| `door_state.view.y_min/y_max` | −3.00 / −0.30 | 문 바깥 |
| `door_state.view.z_min/z_max` | 0.10 / 2.10 | 바닥·문 위 제외 |
| `door_state.window` | 10 | 평균낼 프레임 수 (약 0.5초) |
| `door_state.open_threshold` | 10 | 열림 기준 (점/프레임) |
| `door_state.close_threshold` | 1 | 닫힘 기준 (점/프레임) |

1차 엘리베이터(`elevator_1st.cfg`)는 `close_threshold = 0.5`를 씁니다. S0-3에서 열림 중 평균이 최저 1.2까지 떨어져, 기본값 1로는 여유가 0.2뿐이었기 때문입니다.

**1차 엘리베이터 검증**

| 장면 | 실제 | 결과 |
|---|---|---|
| S0-1_t02 | 3분 닫힘 | OPEN 0 / 3,736, 전환 0 |
| S0-2_t02 | 1분 열림 | OPEN 1,197 / 1,206, 전환 0 |
| S0-3_t01 | 개폐 반복 | 전환 7번, 열림 구간 매번 약 17초 (엘리베이터 대기 시간과 일치) |

문 너머 점: 닫힘 0.0점/프레임, 열림 38.4점/프레임. 열림 중 사람이 문 앞을 가려 3 ~ 5점으로 잠깐 떨어져도 히스테리시스로 OPEN 유지.
임계값은 이 엘리베이터·층 기준이며, 승강장 모양이 다른 층에서는 다시 확인해야 합니다. 반응 지연은 아직 측정 전입니다 (평균 때문에 최대 약 0.5초 예상).

### 사람이 있을 때의 위험 (미검증)

**실제 문 신호가 없어서 이 추정이 문 상태의 유일한 출처**입니다. 1차 녹화는 빈 캐빈이라 아래 두 경우는 아직 확인하지 못했습니다.

| 상황 | 일어날 수 있는 일 | 오판 방향 |
|---|---|---|
| 문 열림, 사람이 **문턱에 오래 서 있음** | 센서 시선이 가려져 문 너머 점이 0 가까이로 떨어짐 (S0-3에서 잠깐 가렸을 때 평균 1.2까지 하락) | 열림 → **닫힘** (위험) |
| 문 닫힘, 사람이 **캐빈 안 문 바로 앞** | 닫힌 금속 문에 비친 사람의 거울상이 문 너머 상자 안에 찍힘 | 닫힘 → 열림 |

**원칙: 애매하면 열림으로 본다.** 열려 있는 문을 닫혔다고 알리는 쪽이 더 위험하므로, `UNKNOWN`도 C에서 열림처럼 다루도록 협의 필요.

**대응 방안**
1. 같은 프레임에 DOOR 구역 덩어리가 있으면 OPEN → CLOSED 전환을 막기 (문턱에 사람이 있는 동안은 열림 유지). `SignalPipeline`에서 DBSCAN 다음에 보정
2. 상자 안 점 개수 대신 **열림 맵에만 있는 칸**(문이 열려야 보이는 승강장 고정 자리)에 찍힌 점을 세기 → 거울상·승강장 통행자 영향 감소. 층마다 열림 맵이 필요
3. 문이 여닫힐 때 문짝 위쪽 끝이 도플러를 띤 DOOR 덩어리로 잡히는 것을 **문 움직임 신호**로 활용 → `OPENING` / `CLOSING` 판단, 전환 확인

## DBSCAN 클러스터링

가까운 점들을 사슬처럼 이어 덩어리로 묶고, 이웃이 적은 외톨이 점은 노이즈(`cluster = -1`)로 표시합니다.
알고리즘은 공통 틀 `Clusterer`를 따르므로, 새 알고리즘도 같은 틀로 만들면 교체할 수 있습니다.

| 설정 키 | 기본값 | 설명 |
|---|---|---|
| `dbscan.epsilon` | 0.30 | 수평 이웃 거리 [m] |
| `dbscan.min_pts` | 10 | 핵심 점이 되기 위한 최소 이웃 수 (자기 자신 포함) |
| `dbscan.z_scale` | 0.2 | 높이 차이 가중치. 위아래 이웃 거리 = epsilon / z_scale (= 1.5m) |

`cluster` 번호는 **프레임마다 새로 매기는 번호**라, 프레임 간에 같은 사람을 이어주는 것은 트래킹(C)의 역할입니다.

### 파라미터 결정 과정 (`front_man`, 사람 1명, 정면 0.5~2.0m 구역)

**문제 발견**: 기본 설정(epsilon 0.2, z_scale 1.0)에서는 사람 1명이 1개 덩어리로 묶인 프레임이 **1.2%**뿐이었다.
덩어리 위치를 보니 x는 같고 z만 −0.48 ~ +1.14로 층층이 나뉘어, 사람이 **다리·몸통·상체로 위아래로 쪼개지고** 있었다.

**해결**: 높이 차이에 가중치(`z_scale`)를 낮게 주어 위아래로는 넉넉하게, 수평으로는 엄격하게 이웃을 판단한다.
엘리베이터 승객은 옆으로는 붙어 서지만 위아래로 겹치지는 않으므로 이 방향이 맞다.

| epsilon \ z_scale | 1.0 | 0.5 | 0.3 | 0.2 |
|---|---|---|---|---|
| 0.20 | 1.2% | 14.0% | — | — |
| 0.25 | — | 36.0% | 68.9% | 89.6% |
| 0.30 | 9.8% | 51.2% | 86.6% | **94.5%** |
| 0.35 | — | 64.0% | 91.5% | 98.8% |

(값: 사람 1명이 정확히 1개 덩어리로 묶인 프레임 비율, min_pts 10)

- **선택: epsilon 0.30, z_scale 0.2, min_pts 10** → 1.2%에서 94.5%로 개선
- epsilon 0.35가 더 높지만, 수평 기준이 넓어지면 옆 사람과 합쳐질 위험이 커져 0.30에서 시작
- min_pts 5와 10은 결과 차이가 거의 없어(사람 점이 빽빽함) 노이즈에 덜 민감한 10 선택
- 처리 시간: 프레임당 약 7 ~ 8ms (최적화 없는 빌드 기준, 50ms 예산 내). Release 재측정 예정
- DBSCAN만으로는 노이즈가 거의 걸러지지 않음(평균 0 ~ 1개) → Step 3과 배경 제거 필요

**센서 판단과 비교 (front_man 첫 프레임)**: 사람 덩어리(571점, 정면 1.37m)의 점이 **전부** 센서 target_id 6과 일치.
같은 프레임이 기본 설정에서는 5개 덩어리로 쪼개졌었음. 나머지 덩어리는 바닥·배경 (target_id는 센서 대상 번호로, 물체에도 붙음).

### 리허설 결과 (`rehearsal.cfg`, 녹화자가 서 있던 센서 옆 y 1.2 이후 제외)

| 장면 | 실제 사람 | 덩어리/프레임 |
|---|---|---|
| R1-1 ~ R1-3 | 1 | 1.00 ~ 1.05 |
| R2-1 | 1 | 0.92 |
| R1-4 (승강장) | 1 | 1.23 |
| **R3-1 (두 사람 나란히)** | **2** | **1.53** (약 절반의 프레임에서 합쳐짐) |
| R0-2 (빈 장면) | 0 | 0.50 |

R1-2와 R1-3 중심 간격이 0.30 m로 나와 테이프 간격(1.0 m)과 다름. 센서 바로 아래 시야 문제 또는 연기자 위치 확인 필요.

### 남은 검증

- 서 있는 사람 옆의 낮은 자세(아이 키 대용)가 분리되는지 → Step 5에서 머리 높이 봉우리 기반 분할
- 누운 사람이 여러 덩어리로 쪼개지는지 (R5) → 낮은 덩어리 병합 후처리 ([docs/dbscan_design.md](docs/dbscan_design.md))
- 위아래 이웃 거리가 1.5m라 바닥 반사점이 다리와 붙을 수 있음 → ROI `z_min`으로 먼저 제거

## 데이터 형식

**입력 CSV** (컬럼은 헤더 이름으로 찾음, 순서 무관)

```
frame,timestamp_us,x,y,z,doppler,power,target_id
```

- 필수: `frame, x, y, z, doppler, power` / 선택: `timestamp_us`(없으면 0), `target_id`(없으면 −1)
- `frame`: 센서 원본 frameCount
- `timestamp_us`: 세션 첫 프레임 기준 경과 시간 [µs, 정수]. 없으면 `timestamp_ms`를 읽어 µs로 반올림 변환
- 단위: x/y/z m, doppler m/s

**출력 CSV (점 단위)**

```
frame,timestamp_us,x,y,z,doppler,power,target_id,zone,cluster
```

- `frame`, `timestamp_us`는 입력 값 그대로 보존
- `zone`: `DOOR` / `CABIN`
- `cluster`: 프레임 안의 덩어리 번호 (0부터), −1은 노이즈
- 파싱할 수 없는 줄은 경고(줄 번호 포함) 후 건너뜀

**문 상태 (`_door.csv`)**

```
frame_id,timestamp_us,state
```

- `state`: `UNKNOWN` / `OPEN` / `CLOSED`

**덩어리 요약 (`_clusters.csv`, B → C 형식)**

```
frame_id,timestamp_us,cluster_id,cx,cy,cz,x_min,x_max,y_min,y_max,z_min,z_max,z_top,mean_doppler,num_points,zone,maybe_merged
```

- 덩어리 하나당 한 줄, 덩어리가 0개인 프레임은 `frame_id,timestamp_us`만 있는 줄
- `z_top`: 점 높이의 상위 95% (가장 가까운 순위 방식, 점 20개 미만이면 사실상 최댓값)
- `mean_doppler`: 센서에 다가오면 −, 멀어지면 + (미니테스트 `doppler_man`으로 확인)
- `zone`: 덩어리 점 중 많은 쪽, 같으면 `DOOR`
- `maybe_merged`: 0 / 1 (지금은 항상 0)

## 설정 파일

`config/elevator_default.cfg`에서 `key = value` 형식으로 관리하며, 엘리베이터마다 복사해서 수정합니다.
값이 없는 키는 코드의 기본값을 사용하고, ROI 범위가 뒤집혀 있으면(min ≥ max) 실행 시 경고 후 종료합니다.
**설정 파일 속 상대 경로는 설정 파일이 있는 폴더 기준**입니다 (`config/` 안의 파일이면 `../data/background/...`).

| 키 | 기본값 | 설명 |
|---|---|---|
| `sensor.tx/ty/tz` | 0 | 센서 설치 위치 [m] |
| `sensor.roll/pitch/yaw_deg` | 0 | 센서 설치 각도 [deg] (위 "각도 이름 주의") |
| `polar.range_res` | 0.071565 | 거리 격자 간격 [m] |
| `polar.uv_res` | 0.015938 | u, v 격자 간격 |
| `door.*` | x ±0.45, y −0.50~0.30, z 0.10~2.10 | Door Zone 범위 |
| `cabin.*` | x ±0.75, y 0.30~1.35, z 0.10~2.20 | Cabin 범위 |
| `background.*` | 위 표 참고 | 정적 배경 제거 |
| `door_state.*` | 위 표 참고 | 문 상태 추정 |
| `dbscan.*` | 위 DBSCAN 표 참고 | 클러스터링 |

Door Zone과 Cabin이 겹치는 경계에서는 진입/퇴장 판단을 위해 **Door Zone을 우선**합니다.
캐빈 범위는 벽보다 조금 안쪽까지만 잡아서 벽 자체의 점과 벽 너머 다중경로 유령 점을 제외합니다.

**1차 엘리베이터 (`elevator_1st.cfg`)**: 캐빈 162 × 137 cm, 문 폭 88 cm, 센서는 x<0 쪽 뒤 모서리
(tx −0.65, ty 1.19, tz 2.14, roll −41, yaw 225). yaw는 현장 측정 44°와, 각도를 바꿔 가며 점이 캐빈에 가장 잘 들어오는 값을 찾은 결과가 일치.

현재는 **승강장(OUTSIDE) 구역이 없어서** y < −0.50인 점은 ROI에서 제거됩니다.
A-D 합의 v4 예시의 승강장 승객(y = −1.20, OUTSIDE)은 지금 범위로는 출력되지 않으므로 `HALL` 구역을 추가할 예정입니다 (아래 "협의 필요" 참고).
원본 PCR에는 모든 점이 남아 있으므로 범위를 바꾼 뒤 다시 실행하면 승강장 데이터를 되살릴 수 있습니다.

## 실측 데이터 특성

- 프레임당 점 수: 미니테스트 `front_man` 약 646개, `right_man` 약 1,020개 / 1차 엘리베이터 빈 캐빈 약 60개 (문 열림 시 약 270개)
- 프레임 속도 약 18.5 ~ 20 fps로 일정하지 않고, 프레임 번호가 가끔 건너뜀 → `timestamp_us`로 실제 간격 사용
- `power`는 dB가 아닌 선형 값으로 보임 (1,066 ~ 301,723) → Step 3 임계값은 로그 변환 후 설계 예정
- `target_id`: 0 ~ 254는 센서가 붙인 대상 번호(녹화마다 다르고 물체에도 붙음), 255는 대상 없음 (A 확인, 특별 취급하지 않음).
  센서 Detection의 status는 0 서 있음 / 1 누움 / 2 앉음 / 3 넘어짐 / 4 판단 불가. 빈 방에서도 물체를 "앉음"으로 추적하므로 참고용으로만 사용

## 알아둘 점

- PCR 변환 도구는 **점이 없는 프레임을 CSV에서 제외**합니다. 그래서 frame 번호가 건너뛰어도 통신 유실인지 빈 프레임인지 CSV만으로는 구분할 수 없고, 배경 맵 점유율 계산의 분모가 실제보다 약간 작아집니다.
- 엘리베이터가 아닌 장소의 데이터는 `config/minitest.cfg`(센서 좌표 그대로, ROI 사실상 해제)로 테스트합니다.
- **녹화 담당은 센서 시야 밖에**: 센서 옆도 시야 안입니다. 리허설에서 센서 옆에 선 녹화자가 모든 장면에 덩어리로 잡혀, 분석에서 y 1.2 m 이후를 잘라내야 했습니다. 엘리베이터에서는 캐빈 밖, 문 정면과 센서 대각선 쪽을 피합니다.
- **캐빈 안 물건은 녹화 내내 같은 자리에**: 1차 녹화의 파워뱅크(x ≈ −0.63, y ≈ 1.0)가 가끔 작은 덩어리로 잡힙니다.
- **설정 파일을 복사해 만든 변형**(`*_wide.cfg`, `rehearsal_floor.cfg`)은 센서 값을 원본과 같게 유지해야 합니다. 리허설에서 `rehearsal_floor.cfg`만 예시값으로 남아 바닥 확인이 틀릴 뻔했습니다.

## 협의 필요

### 어른 옆에 붙은 아이 (덩어리 합쳐짐) - C

**문제**: DBSCAN은 위아래로 넉넉하게 묶도록 튜닝되어(z_scale 0.2, 위아래 1.5m),
아이가 어른 바로 옆에 붙으면 두 사람이 한 덩어리로 합쳐질 수 있다. 리허설 R3-1(어른 두 명 나란히)에서도 약 절반의 프레임에서 합쳐졌다.
z_scale을 키우면 어른 한 명이 다시 위아래로 쪼개지므로 클러스터링 단계에서는 막기 어렵다.

**역할 분담 (C와 합의)**
- B (Step 5): 머리 높이 봉우리로 분할하고 `maybe_merged` 표시
- C (트래킹): 합쳐지기 전부터 따로 추적하던 두 ID를 유지, 시간 흐름으로 최종 판단

### 낮은 키와 낙상 구분 - C

- 아이는 평소 키가 어른이 넘어진 높이와 비슷하므로, 낙상 판단은 절대 높이가 아니라
  **그 사람의 평소 높이에서의 변화량 + 하강 속도** 기준으로 (C에 공유 완료 / 확인 필요)
- B (Step 5): 높이 하한을 0.5 ~ 0.7m로 낮춰 작은 사람을 버리지 않기

### 문 상태(door_state) 전달 - A

- B 추정값 → A → 같은 프레임의 C 전달, 다음 프레임 B의 `ctx.door_state` (A와 합의)
- `door_command`는 C 상태 머신이 결정, B는 문 상태 관측만 담당
- 여러 출처(B 추정, 실제 문 신호, 수동 입력)가 생기면 A가 선택. 출처 구분 칸을 둘지는 협의 필요
- B의 `UNKNOWN`을 A-D `door_state`(OPEN / CLOSED / OPENING / CLOSING)로 어떻게 표시할지 협의 필요

### Door Zone · 승강장(OUTSIDE) 범위 - C, D

**현재 (B 초안)**: Door Zone y −0.50 ~ +0.30, Cabin y +0.30 ~ 뒷벽 안쪽, 승강장 구역 없음
- 문턱(y = 0)을 사이에 두고 양쪽에 걸친 범위라, 캐빈 안이라도 문턱에서 0.3m까지는 `DOOR`
- A-D 합의 v4에서 Door Transition Zone 범위는 "C와 협의 후 확정"으로 남아 있음

**문제**: y < −0.50은 제거되어, C가 승강장 대기 승객을 OUTSIDE로 판정할 데이터가 없음

**제안**
- B: 승강장 쪽 점을 지우지 않고 `HALL` 구역 라벨을 추가해 넘김 (ROI 필터·Zone 종류·출력 라벨 수정)
- C: `HALL` 덩어리를 보고 OUTSIDE 판정 (상태 판정은 C 담당, v4 기준)
- B의 `zone = DOOR` 범위와 C의 Door Transition Zone을 **하나의 범위로 통일**

**정해야 할 것**
- 승강장 쪽 범위 (초안: 문턱에서 약 1.5m까지)
- Door Zone의 캐빈 쪽 깊이 (현재 0.3m)
- D 화면에 구역 경계를 표시할지

## TODO

### 연동 (A)
- [x] `SignalPipeline` 클래스, `elevsafe::sp` 이름 공간, C++20 빌드 확인
- [x] 설정 파일 속 경로를 설정 파일 위치 기준으로 (Backend 실행 위치와 무관하게 배경 맵 로드)
- [x] 도구 실행 파일은 단독 빌드일 때만 (Backend `add_subdirectory` 시 `radar_core`만)
- [x] 레이더로 문 상태 추정 → `PipelineResult.estimated_door_state`
- [ ] A의 Adapter 연결 후, 같은 PCR을 오프라인과 Backend(Emulator 재생)로 처리해 결과 비교
- [ ] Windows(MSVC) 빌드, Release 처리 시간 측정 → 루트 `docs/timing.md`
- [ ] "애매하면 열림" 원칙과 `UNKNOWN` 처리 방식을 C와 합의 (실제 문 신호 없음)

### 다음 엘리베이터 녹화 전
- [ ] FOV 재촬영 계획 (모서리·중앙·문턱·승강장 위치별 Space 기록)
- [ ] S1(사람 1명), S3(승하차), R5(낙상) 시나리오
- [ ] 녹화자 위치 규칙(센서 시야 밖), 캐빈 안 물건 위치 고정
- [ ] S0-3처럼 문 개폐 장면에서 타이머로 문 동작 순간 기록 (문 상태 추정 반응 지연 측정용)
- [ ] 문 상태: DOOR 구역에 덩어리가 있으면 OPEN → CLOSED 전환 막기 (위 "사람이 있을 때의 위험" 1번)
- [ ] 문 상태 위험 장면 시나리오 추가: 문 닫힘 + 캐빈 안 문 앞에 1 ~ 2명 / 문 열림 + 문턱에 5초 이상 서 있기 / 문 열림 + 승강장 여러 명

### 다음 엘리베이터 녹화 후
- [ ] FOV로 위치별 좌표 검증, R1-2·R1-3 간격 문제 원인 확인
- [ ] 승객 장면에서 캐빈 안으로 들어오는 다중경로 유령 덩어리 확인 → Step 3 SOR / RCS 설계
  (power 로그 변환, 거리별 임계값, 약한 power·듬성한 점 제거)
- [ ] 사람이 아닌 덩어리 거르기 (바닥 근처에 점이 없는 덩어리, 누운 자세와 구분)
- [ ] Step 5 봉우리 분할 + `maybe_merged`
- [ ] DBSCAN·배경 맵 파라미터 최종 튜닝, 운행 중 진동(S0-4)으로 dilation 1 충분한지 확인
- [ ] 문 상태 추정 임계값을 다른 층에서 확인
- [ ] 문 상태: 위험 장면으로 오판 확인, 열림 맵 기반 판단·문 움직임 신호(OPENING/CLOSING) 시험

### 이후
- [ ] `HALL` 구역 추가 (ROI 필터, Zone 종류, `zoneToString`, cfg `hall.*`), Door Zone·승강장 범위를 C와 협의 후 확정
- [ ] **배경 맵 자동 학습**: 엘리베이터가 빈 동안(승객 0명 + 문 닫힘 + 움직임 없음) 자동으로 맵 생성·갱신
  → 설치할 때 빈 장면을 사람이 찍을 필요 없게 (여러 엘리베이터 배포 비용 감소)
- [ ] 배경 맵 자동 위치 보정: 맵을 ±1~2칸 옮겨 가장 잘 겹치는 위치 찾기 (재설치·흔들림 대응)
- [ ] 빈 캐빈 녹화로 캐빈 벽·바닥 위치 자동 추정 (설치 실측 보조)
- [ ] DBSCAN 변형 알고리즘 ([docs/dbscan_design.md](docs/dbscan_design.md))
- [ ] 바닥 확인 모드를 실행 옵션으로 (지금은 `rehearsal_floor.cfg`처럼 설정 파일을 따로 둬서 값이 어긋나기 쉬움)