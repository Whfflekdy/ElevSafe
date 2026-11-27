# ElevSafe Frontend

React 기반 엘리베이터 관제 대시보드입니다. 백엔드에서 전달한 승객 위치·상태와 문 상태를 2D 평면도에 표시하고, 수동 문 제어 요청을 전송합니다.

> 이 문서는 최신 코드(`App.jsx`, `App.css`) 및 **A-D WebSocket JSON 데이터 포맷 합의서(v7)**를 기준으로 작성되었습니다.
>
> 현재 기본 실행 모드는 **실제 WebSocket 연동 모드(`USE_REAL_WEBSOCKET = true`)**로 설정되어 있습니다.

--- 

## 1. 사용 기술

| 구분 | 사용 기술 |
|---|---|
| UI 프레임워크 | React (Functional Components, Hooks) |
| 스타일링 | CSS (`App.css`, 상태별 테마 전환 및 애니메이션) |
| 2D 평면도 | SVG 외곽선, React DOM 기반 승객 마커, CSS 실측 비례 문짝 애니메이션 |
| 상태 및 렌더링 최적화 | `useState`, `useEffect`, `useRef`, `requestAnimationFrame` |
| 실시간 통신 | 브라우저 WebSocket API (지수 백오프 자동 재연결 포함) |
| 시뮬레이션 타이머 | `setTimeout`, `clearTimeout` (테스트 모드 전용) |

자동 안전 제어 판단과 승객 추적(Tracking) 알고리즘은 백엔드/추적 모듈에서 수행하고 프론트엔드는 시각화와 수동 명령 전달에 집중합니다.

## 2. 주요 구현 기능

### 대시보드

-  **상단 헤더**: 시스템 타이틀, 레이더 제원 배지(`Range: 12m(Person) / 7m×7m | FoV: 90°(±45°) | Rate: 50ms`), 실시간 프레임 경과 시간(`timestamp_us`), 레이더 데이터 정체 경고 표시
- **좌측 패널 (`승객 상세 정보`)**: 평면도에서 선택한 승객의 ID, 이름(`label` 미전달 시 `Passenger #{id}` 자동 생성), 아이콘이 포함된 대형 상태 카드, `m` 단위 X/Y 좌표, 선택 해제 버튼 제공
- **중앙 패널 (`2D 평면도`)**: 실측 치수와 승강장 압축 비율이 반영된 캐빈·출입구·사다리꼴 연결부·승강장 SVG 평면도, 문 개폐 애니메이션, 승객 위치 마커, 데이터 끊김 오버레이 표시
- **우측 패널 (`능동 안전 제어` & `실시간 시스템 로그`)**: 현재 도어 상태(`OPEN`, `CLOSED`, `OPENING`, `CLOSING`, `UNKNOWN`), 수신된 `door_command`, 수동 제어 버튼(`강제 OPEN` / `강제 HOLD` / `강제 CLOSE`), 최대 50줄까지 유지되는 터미널 로그 표시

### 승객 상태 및 위험 경보 표시

- **정상 승객 (`safety_state: null`)**: 초록색 점(`.normal`)으로 표시되며, 이동 상태(`OUTSIDE`, `ENTERING`, `INSIDE`, `EXITING`)에 따라 상세 패널 라벨이 구분됩니다.
- **위험 승객 (`safety_state: 'FALL'` 또는 `'IMMOBILE'`)**: 빨간색 펄스 애니메이션 점(`.danger`)으로 최상단(`z-index: 100`)에 표시됩니다.
- **전체 경보(배경 전환 및 경고음)**:
  - 현재 조건: `safety_state === 'IMMOBILE'`이면서 `state !== 'OUTSIDE'`인 승객이 1명 이상 존재할 때 활성화됩니다.
  - 활성화 시 대시보드 전체 배경이 붉은 경고 테마(`.danger-background`)로 전환되며 `/alert-sound.mp3` 경고음 재생을 시도합니다(브라우저 자동재생 정책에 따라 제한될 수 있음).
  - `FALL` 상태는 개별 위험 마커로 강조되지만 v7 합의에 따라 전체 화면 경보 및 경고음은 발생시키지 않습니다.

### 통신 건전성 및 레이더 갱신 감시

- **렌더링 최적화 (`requestAnimationFrame`)**: 10Hz(100ms) 주기로 수신되는 데이터를 즉시 리렌더링하지 않고 `latestDataRef`에 버퍼링한 뒤 브라우저 프레임 주기에 맞춰 일괄 반영합니다.
- **자동 재연결 (Exponential Backoff)**: WebSocket 연결이 끊어지면 `1초`부터 시작해 최대 `10초`까지 대기 시간을 2배씩 늘리며 자동으로 재접속을 시도합니다.
- **데이터 끊김 감지 (`isDataLost`)**: 연결 후 초기 데이터가 3초 이상 수신되지 않거나, 마지막 유효 메시지 수신 후 `3초(3000ms)` 이상 새 데이터가 없으면 평면도 중앙에 **`데이터 끊김`** 반투명 띠 오버레이를 표시합니다.
- **레이더 정체 감지 (`isRadarStale`)**: 백엔드 통신은 유지되더라도 `timestamp_us` 값이 `3초(3000ms)` 이상 증가하지 않으면 헤더에 **`⚠️ 레이더 데이터 오래됨`** 경고를 표시하며, `timestamp_us` 감소 시 세션 초기화를 감지해 로그에 기록합니다.

---

## 3. 도면 규격 및 좌표 변환 기준

### 도면 실측 치수 및 화면 왜곡(압축) 설정

```jsx
const WAITING_COMPRESSION = 0.5; // 승강장 세로 화면 압축 비율 (50%)

const CONFIG_CM = {
  ELEV_WIDTH: 162,
  ELEV_DEPTH: 137,
  DOOR_WIDTH: 88,
  DOOR_THICKNESS: 24,
  WAITING_DEPTH: 250 * WAITING_COMPRESSION, // 승강장 길이 (실측 250cm → 화면 125cm)
  WAITING_WIDTH: 166,
  TRAP_DEPTH: 52 * WAITING_COMPRESSION,     // 사다리꼴 연결부 깊이 (실측 52cm → 화면 26cm)
  TRAP_BOTTOM_W: 101,
};

const SCALE = 1.6; // 1cm = 1.6px
```

| 설정 | 의미 | 단위 |
|---|---|---|
| `ELEV_WIDTH` | 캐빈 폭 | cm |
| `ELEV_DEPTH` | 캐빈 깊이 | cm |
| `DOOR_WIDTH` | 출입구 폭 | cm |
| `DOOR_THICKNESS` | 출입구 구간 깊이 | cm |
| `TRAP_DEPTH` | 사다리꼴 연결부 깊이 | cm |
| `TRAP_BOTTOM_W` | 사다리꼴 아래쪽 폭 | cm |
| `WAITING_WIDTH` | 승강장 직사각형 폭 | cm |
| `WAITING_DEPTH` | 사다리꼴 아래 직사각형 승강장 깊이 | cm |
| `SCALE` | 1cm를 화면에 표시할 px 수 | px/cm |


### 공통 좌표계 및 렌더링 위치 계산

A–B–D 공통 좌표계 기준에 따라 백엔드는 **m 단위** `x`, `y` 좌표를 전달합니다.

- **원점 `(0, 0)`**: 엘리베이터 출입구 문턱 정중앙(문 두께의 중심)
- **X축 (좌우)**: 문 쪽을 바라보는 기준 왼쪽 `+` (화면 오른쪽), 오른쪽 `-` (화면 왼쪽)
- **Y축 (전후)**: 캐빈 내부 `+` (화면 위쪽), 승강장 대기 구역 `-` (화면 아래쪽)

```jsx
const x_cm = p.x * 100;
let y_cm = p.y * 100;

// 문 바깥쪽(승강장 영역)인 경우에만 Y좌표 압축 비율 적용
const doorOuterBoundary = -(CONFIG_CM.DOOR_THICKNESS / 2); // -12cm
if (y_cm < doorOuterBoundary) {
  const overDistance = y_cm - doorOuterBoundary;
  y_cm = doorOuterBoundary + (overDistance * WAITING_COMPRESSION);
}

const leftPos = (MAX_WIDTH / 2 + x_cm) * SCALE;
const topPos = (CONFIG_CM.ELEV_DEPTH - y_cm + (CONFIG_CM.DOOR_THICKNESS / 2)) * SCALE;
```


## 4. 실행 모드 및 WebSocket 설정

`src/App.jsx` 상단의 상수로 실행 모드를 제어합니다.

```jsx
const USE_REAL_WEBSOCKET = true;

const RADAR_WS_URL = process.env.REACT_APP_WS_URL
  || (process.env.NODE_ENV === 'production'
    ? `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.host}/radar`
    : 'ws://127.0.0.1:8080/radar');
```

### 1) 실제 연동 모드 (`USE_REAL_WEBSOCKET = true`, 기본값)

- 환경 변수 `REACT_APP_WS_URL`이 지정되어 있으면 해당 주소를 우선 사용합니다.
- 개발 환경(`npm start`)에서는 `ws://127.0.0.1:8080/radar`에 연결합니다.
- 프로덕션 빌드 환경에서는 현재 접속한 호스트의 `/radar` 경로(`ws://` 또는 `wss://`)로 자동 연결합니다.
- 초기 도어 상태는 `'UNKNOWN'`으로 시작하며, 서버에서 수신되는 스냅샷에 따라 승객과 도어 상태를 갱신합니다.

### 2) 로컬 UI 테스트 모드 (`USE_REAL_WEBSOCKET = false`)

- 백엔드 서버 없이 샘플 JSON 데이터로 UI 동작과 도어 애니메이션을 테스트합니다.
- 수동 제어 버튼 클릭 시 실측 기반 애니메이션 시간(`OPEN`: `2.68초`, `CLOSE`: `2.35초`) 후 `OPEN` 또는 `CLOSED` 상태로 전환됩니다.

---

## 5. WebSocket 통신 프로토콜 (v7 기준)

### 백엔드 → 프론트엔드 수신 JSON 스키마

프론트엔드는 수신된 모든 메시지에 대해 엄격한 스키마 유효성 검사를 수행하며, 필수 필드가 누락되거나 허용되지 않은 값이 포함된 메시지는 무시하고 경고 로그를 남깁니다.

```json
{
  "timestamp": "2026-11-01T10:30:00Z",
  "timestamp_us": 9382050,
  "door_command": "HOLD",
  "door_state": "OPEN",
  "passengers": [
    {
      "id": 1,
      "x": -0.20,
      "y": 1.00,
      "state": "INSIDE",
      "safety_state": null,
      "label": "Passenger #1"
    },
    {
      "id": 2,
      "x": 0.15,
      "y": -0.60,
      "state": "ENTERING",
      "safety_state": "FALL",
      "label": "Passenger #2"
    }
  ]
}
```

#### 최상위 필수 필드

| 필드 | 타입 | 허용 값 및 설명 |
|---|---|---|
| `timestamp` | `String` | 비어 있지 않은 시각 문자열 |
| `timestamp_us` | `Integer` | 세션 첫 프레임 기준 경과 시간(`µs`). 레이더 데이터 갱신 정체 감지에 활용 |
| `door_command` | `String \| null` | `null`, `"OPEN"`, `"HOLD"`, `"CLOSE"` (새 명령 없음은 `null`) |
| `door_state` | `String` | `"UNKNOWN"`, `"OPEN"`, `"CLOSED"`, `"OPENING"`, `"CLOSING"` |
| `passengers` | `Array` | 매 프레임 전체 승객 배열 (승객이 없으면 `[]`, 기존 목록을 통째로 교체) |

#### `passengers` 항목 필드

| 필드 | 타입 | 허용 값 및 설명 |
|---|---|---|
| `id` | `Number \| String` | 승객 고유 식별자 (Tracking ID) |
| `x` | `Number` | 공통 좌표계 X 위치 (`m`, 유한한 숫자) |
| `y` | `Number` | 공통 좌표계 Y 위치 (`m`, 유한한 숫자) |
| `state` | `String` | `"OUTSIDE"`, `"ENTERING"`, `"INSIDE"`, `"EXITING"` |
| `safety_state` | `String \| null` | `null`, `"FALL"`, `"IMMOBILE"` (`OUTSIDE`일 때는 항상 `null`) |
| `label` | `String` (선택) | UI 표시 이름. 생략 시 `Passenger #{id}`로 자동 표시 |

### 프론트엔드 → 백엔드 수동 제어 송신 JSON

우측 제어 패널의 수동 버튼을 누르면 동일한 WebSocket 연결로 아래 단일 필드 JSON을 전송합니다.

```json
{
  "door_command": "OPEN"
}
```

- 도어 상태가 `CLOSED` 또는 `CLOSING`일 때는 **`강제 OPEN`** 버튼이, 그 외(`OPEN`, `OPENING`, `UNKNOWN`)일 때는 **`강제 HOLD`** 버튼이 표시되며, **`강제 CLOSE`** 버튼은 항상 표시됩니다.
- `UNKNOWN` 상태에서도 관제사의 수동 `OPEN` / `HOLD` / `CLOSE` 요청을 전송할 수 있습니다.

---

## 6. 도어 상태(`UNKNOWN`) 및 비례 애니메이션 처리

1. **`UNKNOWN` 상태 시각화**:
   - 초기 연결 직후나 레이더 관측 불확실 구간에서 `door_state: "UNKNOWN"`을 수신하면 문짝을 닫힌 상태로 표시하되 문짝 위에 **`문 상태 확인 중`** 텍스트를 표기하여 확정된 `CLOSED` 상태와 구분합니다.
2. **실측 비례 및 방향 전환 보간 애니메이션**:
   - 한쪽 문짝의 최대 이동 거리(`44cm`)를 기준으로 열림 속도(`44cm / 2.68s ≈ 16.42cm/s`)와 닫힘 속도(`44cm / 2.35s ≈ 18.72cm/s`)를 계산합니다.
   - 문이 열리는 도중(`OPENING`)에 닫힘(`CLOSING`) 명령이 들어오거나 그 반대의 경우, `performance.now()` 경과 시간으로 현재 문짝의 이동 거리(cm)를 계산한 뒤 남은 거리에 비례하는 `transition` 시간을 동적으로 산출하여 문이 순간이동하지 않고 자연스럽게 방향을 전환합니다.

---

## 7. 향후 확인 및 보완 예정 사항

- 현재 아직 수정 할 내용 없음.

---



# Getting Started with Create React App

This project was bootstrapped with [Create React App](https://github.com/facebook/create-react-app).

## Available Scripts

In the project directory, you can run:

### `npm start`

Runs the app in the development mode.\
Open [http://localhost:3000](http://localhost:3000) to view it in your browser.

The page will reload when you make changes.\
You may also see any lint errors in the console.

### `npm test`

Launches the test runner in the interactive watch mode.\
See the section about [running tests](https://facebook.github.io/create-react-app/docs/running-tests) for more information.

### `npm run build`

Builds the app for production to the `build` folder.\
It correctly bundles React in production mode and optimizes the build for the best performance.

The build is minified and the filenames include the hashes.\
Your app is ready to be deployed!

See the section about [deployment](https://facebook.github.io/create-react-app/docs/deployment) for more information.

### `npm run eject`

**Note: this is a one-way operation. Once you `eject`, you can't go back!**

If you aren't satisfied with the build tool and configuration choices, you can `eject` at any time. This command will remove the single build dependency from your project.

Instead, it will copy all the configuration files and the transitive dependencies (webpack, Babel, ESLint, etc) right into your project so you have full control over them. All of the commands except `eject` will still work, but they will point to the copied scripts so you can tweak them. At this point you're on your own.

You don't have to ever use `eject`. The curated feature set is suitable for small and middle deployments, and you shouldn't feel obligated to use this feature. However we understand that this tool wouldn't be useful if you couldn't customize it when you are ready for it.

## Learn More

You can learn more in the [Create React App documentation](https://facebook.github.io/create-react-app/docs/getting-started).

To learn React, check out the [React documentation](https://reactjs.org/).

### Code Splitting

This section has moved here: [https://facebook.github.io/create-react-app/docs/code-splitting](https://facebook.github.io/create-react-app/docs/code-splitting)

### Analyzing the Bundle Size

This section has moved here: [https://facebook.github.io/create-react-app/docs/analyzing-the-bundle-size](https://facebook.github.io/create-react-app/docs/analyzing-the-bundle-size)

### Making a Progressive Web App

This section has moved here: [https://facebook.github.io/create-react-app/docs/making-a-progressive-web-app](https://facebook.github.io/create-react-app/docs/making-a-progressive-web-app)

### Advanced Configuration

This section has moved here: [https://facebook.github.io/create-react-app/docs/advanced-configuration](https://facebook.github.io/create-react-app/docs/advanced-configuration)

### Deployment

This section has moved here: [https://facebook.github.io/create-react-app/docs/deployment](https://facebook.github.io/create-react-app/docs/deployment)

### `npm run build` fails to minify

This section has moved here: [https://facebook.github.io/create-react-app/docs/troubleshooting#npm-run-build-fails-to-minify](https://facebook.github.io/create-react-app/docs/troubleshooting#npm-run-build-fails-to-minify)
