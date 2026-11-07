\# kalman-cpp 수정 내역



원본: https://github.com/hmartiro/kalman-cpp (MIT License, Hayk Martirosyan)

수정 대상: kalman.hpp, kalman.cpp



\## 추가

\- `predict()`: 예측 단계만 수행 (측정이 없는 프레임에서 사용)

\- `predict(dt, A, Q)`: 프레임마다 dt, A, Q를 갱신한 뒤 예측

\- `correct(y)`: 보정 단계만 수행 (초기화 여부 확인 포함)

\- 위 세 함수의 선언을 kalman.hpp(public)에 추가



\## 변경

\- `update(y)`: 내부 계산을 직접 하지 않고 `predict(); correct(y);`를 호출하도록 변경 (계산 결과는 동일)

\- 기본 생성자 `KalmanFilter()`: `m, n, t0, t, dt`를 0으로, `initialized`를 false로 초기화 (원본은 초기화하지 않음)



\## 삭제

\- 원본 `update(y)`의 직접 계산 코드 (`x\_hat\_new`를 쓰던 본문). 같은 함수가 중복 정의되어 제거

\- `x\_hat\_new` 멤버는 더 이상 쓰이지 않지만 그대로 남겨 둠



\## 변경 없음

\- 인자 있는 생성자, `init()`, `update(y, dt, A)`, `state()`, `time()`

\- 참고: `update(y, dt, A)`는 Q를 갱신하지 않음. Q까지 바꿔야 하면 `predict(dt, A, Q)` 사용



\## 라이브러리 밖(우리 코드)에서 추가한 것

\- tracking/kalman\_track.hpp: 6상태 \[x, y, z, vx, vy, vz]용 행렬 생성 함수 `makeA`, `makeQ`, `makeTrack` (원본에는 없음)

