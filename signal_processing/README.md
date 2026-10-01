# signal_processing — 신호 처리 및 클러스터링

4D 레이더 포인트 클라우드에서 엘리베이터 출입구(Door Zone)와 내부(Cabin)의 유효 포인트를 정제하고,
클러스터링을 통해 탑승객 단위의 객체를 추출하는 모듈입니다.

담당: 김수연 (B)

## 파이프라인

```
CSV 입력 → 좌표 변환 → ROI 필터 → 정적 배경 제거 → SOR/RCS → DBSCAN → Bounding Box → CSV 출력
           (Step 1)    (Step 1)     (Step 2)       (Step 3)  (Step 4)   (Step 5)
```

- [x] **Step 1**: 센서 → 엘리베이터 좌표 변환, Door Zone / Cabin ROI 필터 및 Zone 라벨링
- [ ] **Step 2**: 정적 배경 제거 (빈 엘리베이터 배경 맵 기반)
- [ ] **Step 3**: 허공 노이즈 제거 (RCS, SOR)
- [ ] **Step 4**: DBSCAN 클러스터링 및 파라미터 튜닝
- [ ] **Step 5**: Bounding Box 추출 및 대형 군집 분할

## 폴더 구조

```
signal_processing/
├── CMakeLists.txt
├── config/
│   └── elevator_default.cfg       # 엘리베이터 규격 / 센서 설치 자세
├── include/                       # 헤더 (선언)
│   ├── common/                    # radar_point.h, box3d.h, config.h
│   ├── io/                        # csv_io.h
│   ├── preprocessing/             # coordinate_transform.h, roi_filter.h
│   ├── clustering/                # (Step 4 예정)
│   └── postprocessing/            # (Step 5 예정)
├── src/                           # 구현 (include와 같은 구조) + main.cpp
├── tests/
├── tools/
└── data/
    ├── raw/                       # 실측 녹화 데이터
    ├── sample/                    # 테스트용 소형 데이터 (tiny.csv)
    └── output/                    # 실행 결과 (git 제외)
```

## 빌드 및 실행

```bash
cd signal_processing
cmake -S . -B build        # 처음 한 번, 또는 CMakeLists.txt 수정 시
cmake --build build
./build/radar_pipeline [config] [input.csv] [output.csv]
```

인자를 생략하면 `config/elevator_default.cfg`, `data/sample/tiny.csv`, `data/output/roi_out.csv`를 사용합니다.

## 좌표계

모든 ROI와 이후 처리는 **엘리베이터 좌표계** 기준입니다.

- 원점: 캐빈 문턱(sill) 중앙의 바닥면
- x: 좌우 (캐빈 안에서 문을 볼 때 오른쪽 +)
- y: 깊이 (캐빈 안쪽 +, 승강장 쪽 −)
- z: 높이 (바닥 0, 위쪽 +)
- 단위: m, deg

변환식: `p_엘리베이터 = R × p_센서 + t`
(R = Rz(yaw) · Ry(pitch) · Rx(roll), t = 엘리베이터 원점 기준 센서 설치 위치)

> **⚠️ 실측 전 확인 필요: 센서 정면 축**
> 회전 부호는 센서 출력 좌표축에 따라 달라집니다.
> - 센서 정면이 +x: 아래로 숙이면 `pitch_deg` 양수
> - 센서 정면이 +y (TI mmWave 출력에서 흔함): 아래로 숙이면 `roll_deg` 음수
>
> 센서 정면에 사람이 섰을 때 x, y 중 어느 값이 크게 나오는지로 확인합니다.

## 데이터 형식

입력 CSV (`target_id`는 생략 가능, 없으면 -1)
```
frame,x,y,z,doppler,power,target_id
```

출력 CSV (ROI 안의 포인트만, `zone` 열 추가)
```
frame,x,y,z,doppler,power,target_id,zone
```

- `zone`: `DOOR` / `CABIN`
- 출력의 `frame`은 원본 번호가 아니라 0부터 매긴 순번입니다.
- 파싱할 수 없는 줄은 경고(줄 번호 포함) 후 건너뜁니다.

## 설정 파일

`config/elevator_default.cfg`에서 `key = value` 형식으로 관리하며, 엘리베이터마다 복사해서 수정합니다.
값이 없는 키는 코드의 기본값을 사용하고, ROI 범위가 뒤집혀 있으면(min ≥ max) 실행 시 경고 후 종료합니다.

| 키 | 기본값 | 설명 |
|---|---|---|
| `sensor.tx/ty/tz` | 0 | 센서 설치 위치 [m] |
| `sensor.roll/pitch/yaw_deg` | 0 | 센서 설치 각도 [deg] |
| `door.*` | x ±0.45, y −0.50~0.30, z 0.10~2.10 | Door Zone 범위 |
| `cabin.*` | x ±0.75, y 0.30~1.35, z 0.10~2.20 | Cabin 범위 |

Door Zone과 Cabin이 겹치는 경계에서는 진입/퇴장 판단을 위해 **Door Zone을 우선**합니다.
현재 값은 폭 1.6m × 깊이 1.4m 캐빈, 폭 0.9m 출입문을 가정한 값이며 실측 후 수정이 필요합니다.

## 참고: 레이더 포인트의 방사형 격자 구조

작년 프로젝트 레포트 기준, 센서 포인트는 연속 공간이 아니라 이산 격자 위에 찍힙니다
(거리 해상도 75mm, 방위각 약 1.04°, 고도각 약 1.27°). 엘리베이터 거리(~2.5m)에서는
거리 방향 간격(7.5cm)이 가장 거칠며, 다음 단계 파라미터에 반영할 예정입니다.

- Step 1: 벽 쪽 ROI 여유를 7.5~10cm 이상으로 검토
- Step 2: 배경 맵을 (거리, 방위각, 고도각) 격자 인덱스 기반으로 구성 검토
- Step 3: SOR 임계값을 거리에 따라 조정
- Step 4: DBSCAN epsilon은 격자 간격보다 충분히 크게 (약 0.15m 이상부터 탐색)

※ 올해 센서/처프 설정이 다르면 수치가 달라지므로 실측 데이터로 재확인 필요

## TODO (실측 데이터 확보 후)

- [ ] 센서 정면 축 확인 및 회전 부호 결정
- [ ] 엘리베이터 치수, 센서 설치 위치 측정 후 `elevator_default.cfg` 반영
- [ ] 빈 엘리베이터 녹화 (Step 2 배경 맵용)
- [ ] 실측 데이터로 ROI 범위 검증