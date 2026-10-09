# ElevSafe Frontend

## 프론트엔드 및 시연

### 현재 구현

- React 기반 ElevSafe 엘리베이터 관제 대시보드 UI 구현
- 엘리베이터 카 내부 및 승강장 대기 구역을 포함한 **2D 평면도 시각화**
  - 실제 치수 기준 설정값 사용 (cm)
  - `ELEV_WIDTH`, `ELEV_DEPTH`, `DOOR_WIDTH`, `WAITING_DEPTH`
  - 화면 표시용 `SCALE` 값으로 m 단위를 픽셀 단위로 변환
- 승객 객체를 점(Point) 형태로 렌더링
  - 승객 위치 `x`, `y` 좌표 기반 표시
  - 승객 상태에 따른 색상 구분
  - 정상 상태: 초록색
  - 위험 상태(`FALL`, `IMMOBILE`): 빨간색
- 승객 점 클릭 시 상세 정보 툴팁 표시
  - 승객 라벨
  - 현재 좌표
  - 상태 정보
- 엘리베이터 문 상태 UI 구현
  - `OPEN`
  - `CLOSED`
  - `OPENING`
  - `CLOSING`
- 수동 문 제어 버튼 구현
  - `강제 OPEN`
  - `강제 HOLD`
  - `강제 CLOSE`
- 문 상태 변경 시 시스템 로그 추가

## 사용 기술

- **Framework**: React
- **Language**: JavaScript (ES6+), JSX
- **Styling**: CSS
- **Visualization**: React DOM 및 CSS 기반 2D 평면도 렌더링
- **State Management**: React `useState`, `useEffect`
- **Demo / Scenario Simulation**: 현재 Mock 승객 데이터 기반 시연, 향후 JSON Time-series Replay 및 Timer API 적용 예정
> 현재 구현은 HTML5 Canvas, WebSocket, 실제 레이더 데이터 통신, 자동 문 제어 로직을 사용하지 않습니다.

## 현재 테스트 데이터

현재 프론트엔드에는 UI 확인을 위한 임시 승객 데이터가 포함되어 있습니다.

```javascript
[
   { id: 1, x: -0.3, y: 1.0, state: 'INSIDE', safety_state: null, label: 'Passenger #1' },
    { id: 2, x: 0.4, y: -0.5, state: 'ENTERING', safety_state: 'FALL', label: 'Passenger #2' },
    { id: 3, x: 0.8, y: 1.3, state: 'INSIDE', safety_state: 'IMMOBILE', label: 'Passenger #3' },
    { id: 4, x: -0.2, y: -1.2, state: 'OUTSIDE', safety_state: null, label: 'Passenger #4' }
  ]
```

- `Passenger #1`: 엘리베이터 내부의 정상 탑승자
- `Passenger #2`: 엘리베이터 입장 중 낙상 상태 승객
- `Passenger #3`: 엘리베이터 내부의 부동 상태 승객
- `Passenger #4`: 엘리베이터 입장 대기 중 승객

위 데이터는 실제 레이더 데이터가 아닌 프론트엔드 시각화 및 위험 상태 표현을 확인하기 위한 예시 데이터입니다.

## 현재 문 제어 방식

현재 문 상태는 자동으로 바뀌지 않습니다.

초기 문 상태는 아래와 같습니다.

```javascript
const [doorState, setDoorState] = useState('CLOSED'));
```

사용자가 화면의 수동 제어 버튼을 눌렀을 때만 문 상태가 변경됩니다.

```javascript
<button onClick={() => handleDoorCommand('OPEN')} className="btn-open">
  강제 OPEN
</button>

<button onClick={() => handleDoorCommand('HOLD')} className="btn-hold">
  강제 HOLD
</button>

<button onClick={() => handleDoorCommand('CLOSE')} className="btn-close">
  강제 CLOSE
</button>

```

현재는 승객의 상태가 `ENTERING`, `EXITING`, `FALL`, `IMMOBILE` 등으로 변경되어도 문 상태가 자동으로 열리거나, 열린 상태를 유지하거나, 닫히지 않습니다.

## 백엔드/레이더 연동 시 데이터 형식

프론트엔드에서는 백엔드가 WebSocket 또는 API를 통해 아래와 같은 형태의 JSON 데이터를 전달하는 것을 기준으로 구현되어 있습니다.

```json
{
  "timestamp": "2026-10-01T14:02:05Z",
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
    }
  ]
}
```

## 프론트엔드에서 현재 사용하는 데이터 변수명

향후 백엔드 또는 레이더 데이터 처리 파트와 연결할 때, 아래 변수명 및 데이터 형식을 함께 맞춰볼 필요가 있습니다.

### 최상위 필드

| 변수명 | 타입 | 설명 |
|---|---|---|
| `timestamp` | String | 데이터 측정 또는 생성 시각 |
| `door_command` | String 또는 null | Backend가 결정한 Door 제어 명령 |
| `door_state` | String | 실제 Door의 현재 상태 |
| `passengers` | Array | 현재 전달할 승객 객체 목록 |


### passengers 내부 필드

| 변수명 | 타입 | 의미 | 비고 |
|---|---|---|---|
| `id` | Number | 승객 추적용 고유 식별자 | 최종 Tracking ID 사용 예정 |
| `x` | Number | 공통 좌표계 X 위치 | 단위 m |
| `y` | Number | 공통 좌표계 Y 위치 | 단위 m |
| `state` | String | 승객 위치/이동 상태 | `OUTSIDE`, `ENTERING`, `INSIDE`, `EXITING` |
| `safety_state` | String 또는 null | 이상 상태 | `FALL`, `IMMOBILE`, `null` |
| `label` | String | UI 툴팁 표시용 이름 | 생략 시 Frontend에서 생성 가능 |

### 승객 위치/이동 상태: `state`

| 값 | 의미 |
|---|---|
| `OUTSIDE` | 승강장 대기 구역에 있으나 아직 탑승 또는 퇴장 전이 중이 아닌 상태 |
| `ENTERING` | 승강장에서 엘리베이터로 입장 중 |
| `INSIDE` | 엘리베이터 내부 탑승 상태 |
| `EXITING` | 엘리베이터에서 퇴장 중 |

### 이상 상태: `safety_state`

| 값 | 의미 |
|---|---|
| `null` | 현재 이상 상태 없음 |
| `FALL` | 낙상 발생 |
| `IMMOBILE` | 장시간 미동 없음 |

### 현재 사용 중인 Door_command 값

| 상태값 | 의미 |
|---|---|
| `null` | 해당 시점에 새로 발생한 Door Command가 없음 |
| `OPEN` | 문이 열린 상태 |
| `HOLD` | 문을 열린 상태로 유지하는 상태 |
| `CLOSE` | 문이 닫힌 상태 |

### door_state 허용 값
| 상태값 | 의미 |
|---|---|
| `OPEN`| 문이 열려있음 |
| `CLOSED` | 문이 닫혀있음 |
| `OPENING` | 문이 열리는 중 |
| `CLOSING` | 문이 닫히는 중|


## 향후 시연 시나리오

최종 목표는 엘리베이터 이용자의 상황을 레이더로 감지하고, 해당 상황에 맞춰 문 제어 상태를 결정하는 것입니다.

예상 시연 흐름은 다음과 같습니다.

1. 승강장에서 사람이 엘리베이터에 접근
2. 사람이 입장하는 동안 문을 열거나 열린 상태로 유지
3. 탑승 인원이 증가하는 상황을 실시간으로 시각화
4. 탑승자가 엘리베이터 내부에서 낙상하거나 쓰러져 장시간 움직이지 않는 상황 감지
5. 위험 상태를 빨간색 경고로 표시
6. 위험 상황에서는 문이 닫히지 않도록 `DOOR_HOLD` 상태 유지
7. 위험 상황이 해소되거나 탑승/하차가 완료된 뒤 문 닫힘 처리

## 미구현 또는 미확인

- 실제 레이더 데이터 수신
- 백엔드와 프론트엔드 간 통신 연결
- 실제 승객 데이터로 `passengers` 상태 갱신
- 레이더 좌표와 프론트엔드 평면도 좌표 간 변환
- 레이더 또는 백엔드의 객체 ID와 프론트엔드 `id` 값 연결
- 승객 상태에 따른 자동 문 제어 로직
  - 사람이 입장 중이면 문 열기 또는 열림 유지
  - 사람이 하차 중이면 문 열림 유지
  - 사람이 모두 탑승하거나 하차한 뒤 문 닫기
  - 낙상 또는 장시간 미동 상태에서는 문 닫힘 방지
- 실제 엘리베이터 문 제어 시스템과의 연동
- 위험 상황 발생 시 자동 로그 기록
- 실제 데이터 수신 오류 및 연결 해제에 대한 UI 처리
- 다수 승객 추적 시 승객 ID 유지
- 시연용 시간 흐름 기반 데이터 재생 기능

## 프론트엔드 역할

프론트엔드는 레이더와 백엔드에서 처리된 사람 감지 및 상태 데이터를 받아 다음 역할을 수행합니다.

- 엘리베이터 내부와 승강장 주변의 사람 위치를 직관적으로 시각화
- 입장, 탑승, 하차, 낙상, 장시간 미동 등의 상태를 실시간 표시
- 위험 상황을 강조하여 관제자가 즉시 확인할 수 있도록 지원
- 엘리베이터 문 상태를 표시
- 향후 백엔드의 판단 결과에 따라 문 열기, 열림 유지, 닫힘 상태를 화면에 반영
- 수동 제어 기능을 통해 비상 시 문 상태를 조작할 수 있는 UI 제공



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
