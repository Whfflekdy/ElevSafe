# ElevSafe Frontend

React 기반 엘리베이터 관제 대시보드입니다. 백엔드에서 전달한 승객 위치·상태와 문 상태를 2D 평면도에 표시하고, 수동 문 제어 요청을 전송합니다.

> 이 문서는 대화에서 마지막으로 제공한 **웹소켓 연동 안내 주석 및 모드 전환 설정이 포함된 수정본**을 기준으로 합니다. 초기 첨부 코드와는 다릅니다.
>
> 기본 설정은 테스트 모드입니다. WebSocket 수신·송신 코드는 준비되어 있지만 실제 백엔드 연결, 실제 레이더 데이터 및 문 제어 시스템과의 연동은 검증 전입니다.

## 1. 사용 기술

| 구분 | 사용 기술 |
|---|---|
| UI | React, JavaScript, JSX |
| 스타일 | CSS |
| 평면도 | SVG 외곽선, React DOM 승객 점, CSS 문 애니메이션 |
| 상태 및 참조 관리 | `useState`, `useEffect`, `useRef` |
| 통신 | 브라우저 WebSocket API |
| 테스트 문 동작 | `setTimeout`, `clearTimeout` |

HTML5 Canvas는 사용하지 않습니다. 자동 안전 제어 판단과 승객 추적 알고리즘은 프론트엔드의 역할이 아닙니다.

## 2. 현재 구현

### 대시보드

- 좌측: 선택한 승객의 상세 정보
- 중앙: 캐빈·출입구·사다리꼴 연결부·승강장 평면도
- 우측: 문 상태, 제어 명령 표시, 수동 제어 버튼, 시스템 로그

### 평면도

- 실측 치수 설정을 이용해 외곽선을 SVG로 표시
- 기존 하늘색 외곽선과 회색 문짝 유지
- 내부 면 채우기와 구역 구분선·점선 없이 외곽선만 표시
- `캐빈 내부 (Cabin)`, `승강장 (Waiting Area)` 이름 표시
- 문 열림·닫힘 애니메이션 표시
- 승객 점 클릭 시 좌측 패널에서 라벨, 좌표, 상태 확인

### 승객 위험 표시

- `safety_state`가 `null`인 승객: 초록색 점
- `FALL` 또는 `IMMOBILE`: 빨간색 점
- 전체 경보 배경 및 경고음: 현재 코드에서는 **OUTSIDE가 아닌 IMMOBILE**이 있는 경우 활성화
- 경고음은 `/alert-sound.mp3`를 사용하며, 브라우저 자동재생 정책에 따라 재생이 제한될 수 있음
- 전체 경보가 비활성에서 활성으로 바뀌면 재생을 시도하며, 각 위험 승객마다 개별 경고음을 발생시키는 구조는 아님

> 점의 위험 색상과 전체 경보는 별도 조건입니다. FALL이라고 무조건 전체 경고음이 발생하지 않습니다. OUTSIDE의 안전 정책은 추가 합의가 필요합니다.

## 3. 치수 및 좌표 표시

### 도면 설정

```jsx
const CONFIG_CM = {
  ELEV_WIDTH: 162,
  ELEV_DEPTH: 137,
  DOOR_WIDTH: 88,
  DOOR_THICKNESS: 24,
  WAITING_DEPTH: 152,
  WAITING_WIDTH: 166,
  TRAP_DEPTH: 52,
  TRAP_BOTTOM_W: 101,
};

const SCALE = 1.5;
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

`WAITING_DEPTH: 152`는 기존 코드의 값을 유지한 것으로, 해당 구간의 실측 확정값으로 보지 않습니다.

### 위치 데이터 변환

백엔드에서 받는 `x`, `y`는 **m 단위 숫자**입니다. 프론트에서 cm로 변환하고 화면 배율을 적용합니다.

```jsx
const x_cm = p.x * 100;
const y_cm = p.y * 100;

const leftPos = (MAX_WIDTH / 2 + x_cm) * SCALE;
const topPos = (
  CONFIG_CM.ELEV_DEPTH + CONFIG_CM.DOOR_THICKNESS / 2 - y_cm
) * SCALE;
```

현재 렌더링 기준:

- `(0, 0)`: 출입구 가로 중앙·문 두께 중앙에 대응
- `x` 양수: 화면 오른쪽
- `y` 양수: 캐빈 안쪽, 화면 위쪽
- `y` 음수: 승강장 방향, 화면 아래쪽

공통 좌표계 변환은 담당자 간 합의한 기준을 사용합니다. 프론트에서 센서 설치 각도·위치 기반 변환을 별도로 수행하지 않습니다. 좌표 설명은 관찰 방향에 따라 혼동되지 않도록 공통 도면 기준으로 통일합니다.

## 4. 실행 모드

파일 상단에서 모드를 선택합니다.

```jsx
const USE_REAL_WEBSOCKET = false;
const RADAR_WS_URL = 'ws://localhost:8080/radar';
```

### 테스트 모드: `false`

- WebSocket에 연결하지 않음
- 코드에 포함된 예시 승객 표시
- 초기 문 상태는 `CLOSED`
- 수동 버튼으로 문 상태를 시뮬레이션
- 실제 백엔드나 문 제어 시스템으로 명령을 보내지 않음

| 버튼 | 테스트 동작 |
|---|---|
| OPEN | `OPENING`으로 변경 후 1.2초 뒤 `OPEN` |
| HOLD | `OPEN`으로 변경 |
| CLOSE | `CLOSING`으로 변경 후 1.2초 뒤 `CLOSED` |

새 명령을 누르면 이전 테스트 타이머를 취소합니다. 테스트 데이터의 `state` 또는 `safety_state`만 바뀌어도 자동 문 제어가 실행되는 것은 아닙니다.

### 실제 연동 모드: `true`

```jsx
const USE_REAL_WEBSOCKET = true;
const RADAR_WS_URL = 'ws://실제서버주소/radar';
```

- 빈 승객 목록으로 시작
- 지정된 WebSocket에 연결
- 수신한 `passengers`로 승객 목록 갱신
- 수신한 `door_state`로 문 표시 갱신
- 수동 버튼은 요청을 전송하고 실제 문 상태는 서버 수신값을 기다림
- 테스트용 문 상태 변경 및 타이머는 실행하지 않음

웹소켓 영역의 주석을 해제하거나 테스트 코드를 삭제할 필요는 없습니다. 모드와 주소를 바꾸면 됩니다.

> `localhost`는 프론트를 실행하는 컴퓨터 자신입니다. 백엔드가 다른 컴퓨터에 있으면 해당 서버 주소를 사용해야 합니다. HTTPS 페이지에서는 일반적으로 `wss://` 연결이 필요합니다. 주소, 인증, 메시지 형식 등은 백엔드와 맞춰야 합니다.

## 5. 테스트 승객 데이터

마지막 수정본의 테스트 데이터는 다음과 같습니다. 위치·상태가 실제 상황을 정확히 재현하는 시나리오가 아니라 UI 확인용 고정 예시입니다.

```jsx
[
  { id: 1, x: 0, y: 1, state: 'INSIDE', safety_state: null, label: 'Passenger #1' },
  { id: 2, x: 0.25, y: 1, state: 'INSIDE', safety_state: null, label: 'Passenger #2' },
  { id: 3, x: 0, y: 0, state: 'INSIDE', safety_state: 'FALL', label: 'Passenger #3' },
  { id: 4, x: -0.25, y: 1, state: 'OUTSIDE', safety_state: 'FALL', label: 'Passenger #4' }
]
```

예를 들어 ID 4는 좌표와 `OUTSIDE` 상태가 공간적으로 일치하지 않습니다. 프론트는 좌표로 점을 그리고 전달된 상태를 표시할 뿐, 둘을 자동 보정하지 않습니다. 실제 시연 전에는 좌표와 상태가 일치하는 데이터로 교체해야 합니다.

## 6. 백엔드 → 프론트 수신 JSON

```json
{
  "timestamp": "2026-10-01T14:02:05Z",
  "door_command": "HOLD",
  "door_state": "OPEN",
  "passengers": [
    {
      "id": 1,
      "x": -0.2,
      "y": 1.0,
      "state": "INSIDE",
      "safety_state": null,
      "label": "Passenger #1"
    }
  ]
}
```

### 최상위 필드

| 필드 | 타입 | 의미 및 현재 처리 |
|---|---|---|
| `timestamp` | String | 측정 또는 생성 시각. 현재 화면 표시·지연 판정에는 사용하지 않음 |
| `door_command` | String 또는 null | 백엔드가 결정한 제어 명령. 허용 값이면 표시 상태 갱신 |
| `door_state` | String | 실제 문 상태. 허용 값이면 문 표시 갱신 |
| `passengers` | Array | 현재 구현은 전체 승객 목록으로 간주하여 교체 |

### 승객 필드

| 필드 | 타입 | 의미 |
|---|---|---|
| `id` | Number 기준, 최종 타입 협의 필요 | Tracking 승객 식별자. 마지막 수신 코드는 숫자·문자열 모두 허용하지만 실제 통신에서는 타입을 고정해야 함 |
| `x` | Number | 공통 좌표계 X, m |
| `y` | Number | 공통 좌표계 Y, m |
| `state` | String | `OUTSIDE`, `ENTERING`, `INSIDE`, `EXITING` |
| `safety_state` | String 또는 null | `FALL`, `IMMOBILE`, `null` |
| `label` | String, 생략 가능 | 표시용 이름. 생략 시 기본 이름 생성은 프론트 보완 필요 |

현재 수신 코드는 JSON 객체 여부와 일부 필드를 검사합니다. 승객 목록에서는 숫자 또는 문자열 ID와 유한한 숫자 좌표를 가진 항목만 남깁니다. 승객 상태 허용 값과 중복 ID 등 전체 스키마를 완전히 검증하는 것은 아닙니다.

### `passengers` 수신 처리 — 구현됨, 팀 확인 필요

| 수신 내용 | 현재 프론트 동작 |
|---|---|
| 승객이 들어 있는 배열 | 유효성 검사 후 전체 목록 교체 |
| `"passengers": []` | 모든 승객 점 제거 |
| `passengers` 필드 생략 | 기존 목록 유지 |
| `passengers`가 배열이 아님 | 해당 필드로 목록을 갱신하지 않음 |

변경된 승객만 보내는 부분 갱신 방식은 구현하지 않았습니다. 백엔드도 전체 목록을 보내는지 확인해야 합니다.

## 7. `door_command`와 `door_state`의 구분

### `door_command`: 무엇을 하라는 명령인가?

| 값 | 의미 |
|---|---|
| `OPEN` | 문을 열라는 명령 |
| `HOLD` | 문을 열린 상태로 유지하라는 명령 |
| `CLOSE` | 문을 닫으라는 명령 |
| `null` | 새 명령 없음이라는 의미로 사용하는 방향. 최종 의미 확인 필요 |

### `door_state`: 문이 지금 실제로 어떤 상태인가?

| 값 | 의미 |
|---|---|
| `OPEN` | 완전히 열림 |
| `CLOSED` | 닫힘 |
| `OPENING` | 열리는 중 |
| `CLOSING` | 닫히는 중 |

예를 들어 다음 데이터는 새 명령은 없고 실제 문은 열려 있다는 뜻으로 해석합니다.

```json
{
  "door_command": null,
  "door_state": "OPEN"
}
```

- 명령 표시값은 `null`로 갱신됨
- 문 모습은 `door_state: OPEN`을 따름
- 과거 OPEN 명령을 별도 상태로 보관하는 기능은 없음
- 명령 이력 저장이나 반복 OPEN의 이벤트 구분은 현재 요구사항에 포함하지 않음

현재 UI의 명령 없음 표시는 `null (없음)`입니다. 표시 제목이 `최근 제어 명령`이어도 별도 과거 이력을 뜻하지 않습니다. 혼동을 줄이기 위해 추후 `수신 제어 명령`으로 변경할 수 있습니다.

> 마지막 수정본에서는 수동 요청 시와 서버 명령 수신 시 동일한 `doorCommand` 상태를 갱신합니다. 둘을 명확히 구분하는 변경은 아직 적용하지 않았습니다. 송신 요청 규격 협의와 함께 정리할 부분입니다.

## 8. 프론트 → 백엔드 수동 요청

현재 구현된 임시 송신 형식:

```json
{
  "door_command": "OPEN"
}
```

`OPEN`, `HOLD`, `CLOSE` 요청을 보냅니다. 연결이 열려 있지 않으면 전송하지 않고 실패 로그를 남깁니다.

이 형식은 **최종 합의 전 구현**입니다. 백엔드가 정한 규격이 있으면 `handleDoorCommand`의 송신 부분을 맞춰야 합니다.

- 송신 로그는 서버 처리 성공 또는 실제 문 동작 완료를 의미하지 않음
- 접수·거절 응답 및 요청 ID 연결은 아직 구현하지 않음
- 버튼 비활성화는 필수가 아님
- 버튼 활성 여부와 무관하게 백엔드가 최종 허용 여부를 판단해야 함
- 수동 요청과 자동 안전 제어의 우선순위는 추가 합의 필요

## 9. 시스템 로그

현재 로그에 기록하는 항목:

- 테스트 모드 안내 또는 실제 모드 연결 대기
- WebSocket 연결 완료·종료·오류
- 수신 JSON 처리 오류
- 수동 명령 송신, 연결되지 않아 전송 불가, 전송 실패
- 테스트 모드의 수동 명령

현재 자동으로 기록하지 않는 항목:

- 수신한 모든 `door_command`
- `door_command: null` 수신
- 모든 `door_state` 변경
- 승객의 모든 위치·상태 변경
- 위험 상황 발생 이력

따라서 **문 상태가 바뀔 때마다 로그가 자동 추가되는 구조는 아닙니다.**

## 10. 문 애니메이션 시간

현재 CSS 문 애니메이션과 테스트 모드의 상태 완료 타이머는 1.2초입니다.

향후 실제 문이 열리고 닫히는 시간을 직접 확인해 조정합니다.

- CSS의 문짝 `transition` 시간 조정
- 열림·닫힘 시간이 다르면 각각 적용하도록 코드 수정
- 테스트 모드의 완료 타이머도 필요하면 함께 조정
- 실제 모드는 타이머가 아니라 서버의 `door_state`로 상태 확정

현재 요구사항에서는 문 개방률 필드를 별도로 요청하지 않습니다. 애니메이션은 실제 개방률의 정확한 실시간 측정 표시가 아닙니다.

## 11. 역할 구분

| 담당 | 역할 |
|---|---|
| C Tracking / State Machine | 승객 ID 유지·종료 및 이동 상태 판정 |
| A Backend / 제어 계층 | 판정 결과 전달, 자동 제어 명령과 실제 문 상태 전달, 수동 요청 허용·실행 처리 |
| Frontend | 위치·상태 시각화, 실제 문 상태 표시, 수동 요청 전송, 통신 및 요청 결과 안내 |

프론트에서 승객 좌표만 보고 `ENTERING`/`EXITING`을 재판정하거나, 안전 상태만 보고 자동 문 제어 명령을 만들지 않습니다.

기존 README의 "위험 상황이면 항상 HOLD"는 현재 합의 내용을 정확히 나타내지 않으므로 사용하지 않습니다. 내부 낙상과 출입 중 낙상 등 상황별 제어 정책은 통신 규약 및 백엔드/상태 머신 판단을 따릅니다. 허용 명령 값은 `HOLD`이며 별도 `DOOR_HOLD` 값은 사용하지 않습니다.

## 12. 연동 전 확인할 사항

- [ ] 수동 요청 JSON 및 접수·거절 응답 규격
- [ ] 수동 제어와 자동 안전 제어 우선순위
- [ ] `passengers`가 전체 목록인지 확인
- [ ] 실제 WebSocket 주소, 인증·구독 필요 여부
- [ ] 주기 전송 또는 변화 시 전송 여부
- [ ] 무수신 시 데이터 지연 판단 기준 및 연결 확인 방식
- [ ] Tracking ID 타입, 일시 미검출 유지·종료, 재검출·ID 재사용 기준
- [ ] OUTSIDE의 FALL/IMMOBILE 경보 및 문 제어 정책
- [ ] `door_command: null`이 새 명령 없음을 뜻하는지 확인
- [ ] 직접 측정한 문 열림·닫힘 시간으로 애니메이션 조정

승객이 없는 것과 통신이 멈춘 것은 다릅니다. 변화 시에만 전송하는 서버라면 무수신만으로 장애라고 판단할 수 없습니다. 전송 방식에 맞춰 별도로 정해야 합니다.

## 13. 구현되어 있으나 검증 전인 부분

- WebSocket 연결 및 JSON 수신
- 실제 수신 데이터로 승객 목록과 문 상태 갱신
- 수동 명령 JSON 전송
- 실제 레이더·Tracking 결과와 화면 위치·ID의 일치
- 실제 문 제어 시스템 동작과 `door_state`의 일치

## 14. 미구현 및 보완 사항

- 자동 재연결, heartbeat 및 데이터 지연 UI
- 수동 요청 접수·거절 응답 처리
- 서버 명령과 수동 요청의 상태·표시 분리
- label 생략 시 기본 이름 생성
- timestamp 활용 및 전체 수신 스키마 검증
- 위험 상황·문 상태 변경의 자동 로그 기록
- JSON 시계열 재생 기반 시연
- 실제 동작 시간에 맞춘 열림·닫힘 애니메이션 조정

## 15. 시연 확인 순서

1. 테스트 모드에서 도면, 문 애니메이션, 승객 클릭 및 상태 표시 확인
2. 좌표·상태가 일치하는 시나리오 데이터 준비
3. 서버 주소와 통신 규격 확정 후 실제 연동 모드 활성화
4. 빈 승객 목록, 다수 승객, 목록 삭제 동작 확인
5. 이동 상태·위험 상태와 문 상태 수신 표시 확인
6. 수동 요청의 정상 접수 및 안전 조건에 따른 거절 확인
7. 연결 오류·종료 및 재접속 처리 확인
8. 실제 측정 시간에 맞춰 애니메이션 조정

> 이 README는 구현 상태와 향후 합의 사항을 구분하기 위한 문서입니다. 연동 코드가 준비되었다는 사실이 실제 서버·레이더·문 제어 시스템과의 정상 동작 검증 완료를 뜻하지는 않습니다.



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
