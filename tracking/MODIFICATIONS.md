# ElevSafe C++ 트래킹 · 상태머신 · Kalman 수정 내역

## 1. kalman-cpp 수정 내역
원본: https://github.com/hmartiro/kalman-cpp (MIT License, Hayk Martirosyan)
수정 대상: `kalman.hpp`, `kalman.cpp`

### 추가
- `predict()`: 예측 단계만 수행 (측정이 없는 프레임에서 사용)
- `predict(dt, A, Q)`: 프레임마다 `dt`, `A`, `Q`를 갱신한 뒤 예측
- `correct(y)`: 보정 단계만 수행 (초기화 여부 확인 포함)
- 위 세 함수의 선언을 `kalman.hpp` (`public`)에 추가

### 변경
- `update(y)`: 내부 계산을 직접 하지 않고 `predict(); correct(y);`를 호출하도록 변경 (계산 결과는 동일)
- 기본 생성자 `KalmanFilter()`: `m, n, t0, t, dt`를 0으로, `initialized`를 `false`로 초기화 (원본은 초기화하지 않음)

### 삭제
- 원본 `update(y)`의 직접 계산 코드 (`x_hat_new`를 쓰던 본문). 같은 함수가 중복 정의되어 제거
- `x_hat_new` 멤버는 더 이상 쓰이지 않지만 그대로 남겨 둠

### 변경 없음
- 인자 있는 생성자, `init()`, `update(y, dt, A)`, `state()`, `time()`
- 참고: `update(y, dt, A)`는 `Q`를 갱신하지 않음. `Q`까지 바꿔야 하면 `predict(dt, A, Q)` 사용

### 라이브러리 밖(우리 코드)에서 추가한 것
- `tracking/kalman_track.hpp`: 6상태 `[x, y, z, vx, vy, vz]`용 행렬 생성 함수 `makeA`, `makeQ`, `makeTrack` (원본에는 없음)

---

## 2. 트래킹 및 상태머신 파라미터·로직 조정 내역 (0~3차 검증 반영)

### 트래킹 (`id_manager.hpp`)
- `n_init = 17` (`10 -> 20 -> 17` 조정): 벽면 다중 경로 반사 등 단기 노이즈 트랙(16프레임 수준)을 억제하면서도, 2인 근접 및 간격을 좁혀 연이어 탑승하는 시나리오에서 두 번째 트랙의 확정 누락을 방지
- `min_points = 21`, `gate_xy = 0.55`, `door_only_n_init = 50`, `seen_non_door` 구역 승격 조건 유지: 빈 캐빈·문 개폐·매트 배경 유령 ID 차단 및 다중 승객 ID 유지

### 상태머신 (`state_machine.hpp`)
- 낙상 판정 임계값: `fall_window_sec = 0.9`, `fall_drop_m = 0.72`, `fall_speed_mps = 1.05` 유지
- `fallCondition()` 하강 도달 조건 강화: 급하강(`drop`, `fall_speed_mps`) 조건에 더해 기준 높이 대비 실제 하강 폭(`z <= k.baseline - cfg_.fall_drop_m`) 조건을 반영
- `pending_fall_t` (`0.8s` 지연 검증) 추가: 급하강 조건 충족 직후 `low` 상태가 `0.8s` 이상 유지될 때만 `FALL`을 확정하고, 보행 교차·밀착 시 발생하는 0.1~0.3s 수준의 순간 `z_top` 스파이크는 즉시 취소하여 오탐 제거
- `resetSafety()` 초기화 항목에 `pending_fall_t = -1` 추가
```[cite: 11]

