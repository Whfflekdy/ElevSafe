# ElevSafe Frontend

## 프론트엔드 및 시연

### 현재 구현

- React 기반 ElevSafe 엘리베이터 관제 대시보드 UI 구현
- 엘리베이터 카 내부 및 승강장 대기 구역을 포함한 **2D 평면도 시각화**
  - 실제 치수 기준 설정값 사용 (cm)
  - `ELEV_WIDTH`, `ELEV_DEPTH`, `DOOR_WIDTH`, `WAITING_DEPTH`
  - 화면 표시용 `SCALE` 값으로 cm 단위를 픽셀 단위로 변환
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
  - `DOOR_OPEN`
  - `DOOR_HOLD`
  - `DOOR_CLOSE`
- 수동 문 제어 버튼 구현
  - `HOLD 강제`
  - `CLOSE 강제`
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
  { id: 1, x: 100, y: 80, state: 'INSIDE', label: 'Passenger #1' },
  { id: 2, x: 105, y: 85, state: 'FALL', label: 'Passenger #2' },
  { id: 3, x: 100, y: 220, state: 'ENTERING', label: 'Passenger #3' }
]
```

- `Passenger #1`: 엘리베이터 내부의 정상 탑승자
- `Passenger #2`: 엘리베이터 내부의 낙상 상태 승객
- `Passenger #3`: 승강장 대기 구역에서 입장 중인 승객

위 데이터는 실제 레이더 데이터가 아닌 프론트엔드 시각화 및 위험 상태 표현을 확인하기 위한 예시 데이터입니다.

## 현재 문 제어 방식

현재 문 상태는 자동으로 바뀌지 않습니다.

초기 문 상태는 아래와 같습니다.

```javascript
const [doorStatus, setDoorStatus] = useState('DOOR_HOLD');
```

사용자가 화면의 수동 제어 버튼을 눌렀을 때만 문 상태가 변경됩니다.

```javascript
<button onClick={() => handleManualControl('DOOR_HOLD')}>
  HOLD 강제
</button>

<button onClick={() => handleManualControl('DOOR_CLOSE')}>
  CLOSE 강제
</button>
```

현재는 승객의 상태가 `ENTERING`, `EXITING`, `FALL` 등으로 변경되어도 문 상태가 자동으로 열리거나, 열린 상태를 유지하거나, 닫히지 않습니다.

## 백엔드/레이더 연동 시 데이터 형식

프론트엔드에서는 백엔드가 WebSocket 또는 API를 통해 아래와 같은 형태의 JSON 데이터를 전달하는 것을 기준으로 구현되어 있습니다.

```json
{
  "timestamp": "2026-10-01T14:02:05Z",
  "door_status": "DOOR_HOLD",
  "passengers": [
    {
      "id": 1,
      "x": 150,
      "y": 120,
      "state": "INSIDE",
      "label": "Passenger #1"
    }
  ]
}
```

## 프론트엔드에서 현재 사용하는 데이터 변수명

향후 백엔드 또는 레이더 데이터 처리 파트와 연결할 때, 아래 변수명 및 데이터 형식을 함께 맞춰볼 필요가 있습니다.

| 변수명 | 타입 | 설명 |
|---|---|---|
| `passengers` | Array | 화면에 표시할 승객 객체 목록 |
| `id` | Number | 각 승객 객체를 구분하기 위한 식별자 |
| `x` | Number | 평면도 내 승객의 가로 위치 |
| `y` | Number | 평면도 내 승객의 세로 위치 |
| `state` | String | 승객의 현재 행동 또는 위험 상태 |
| `label` | String | 툴팁에 표시할 승객 이름 |
| `doorStatus` | String | 현재 화면에 표시할 엘리베이터 문 상태 |
| `selectedDot` | Number 또는 `null` | 현재 클릭되어 상세 정보가 표시된 승객의 `id` |
| `systemLogs` | Array | 화면의 시스템 로그 영역에 표시할 문자열 목록 |

### 현재 사용 중인 승객 상태값

| 상태값 | 의미 | 화면 표시 |
|---|---|---|
| `ENTERING` | 엘리베이터에 입장 중 | 정상 상태 |
| `INSIDE` | 엘리베이터 내부에 탑승 중 | 정상 상태 |
| `EXITING` | 엘리베이터에서 하차 중 | 정상 상태 |
| `FALL` | 낙상 감지 상태 | 위험 상태 |
| `IMMOBILE` | 장시간 움직임 없음 | 위험 상태 |

### 현재 사용 중인 문 상태값

| 상태값 | 의미 |
|---|---|
| `DOOR_OPEN` | 문이 열린 상태 |
| `DOOR_HOLD` | 문을 열린 상태로 유지하는 상태 |
| `DOOR_CLOSE` | 문이 닫힌 상태 |


> 현재 변수명은 프론트엔드에서 임시로 정의한 규약입니다. 백엔드에서 레이더의 `x`, `y`, `z`, `doppler`, `power`, `targetId` 등을 어떤 이름과 단위로 가공해 전달할지 팀 전체에서 확정한 뒤, 프론트엔드 변수명 또는 변환 로직을 맞춰야 합니다.

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
- 레이더/백엔드에서 판단한 사람 상태와 프론트엔드 `state` 값 통일
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
