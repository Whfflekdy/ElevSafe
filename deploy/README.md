# deploy — 학교 서버 중계 배포

현장 제어(센서 → B → C → 문 제어)는 **현장 노트북**에서 하고, 학교 서버에는 결과를 나눠 주는 **중계 서버(relay)**와 **보기 전용 웹**만 올립니다. (4차 회의 결정)

```text
[현장 노트북]  백엔드 ──(로컬 WebSocket)──> bridge
                                              │ 인터넷
[학교 서버]                                   ▼
               nginx :3000 ── /ingest ──> relay ── /radar ──> 여러 화면 (보기 전용)
                          └── /       ──> 프론트 화면
```

- 서버가 끊겨도 현장 노트북의 처리와 문 제어는 멈추지 않습니다. (bridge만 끊김)
- 화면에서 보낸 문 명령은 relay가 버립니다. 원격 화면으로는 문을 움직일 수 없습니다.

## 파일

| 파일 | 역할 |
|---|---|
| `docker-compose.yml` | relay + web 묶음. 이름 `elevsafe`, 메모리·CPU·로그 한도 설정 |
| `relay/relay.js` | 노트북(`/ingest`, 키 필요) → 화면들(`/radar`)로 JSON 전달 |
| `web/Dockerfile`, `web/nginx.conf` | 프론트 빌드 결과 제공, `/radar`·`/ingest`는 relay로 전달 |
| `bridge/bridge.js` | 노트북 백엔드 → 서버 relay 전달 (백엔드에 직접 연결 기능이 생기면 대체) |
| `bridge/fake_backend.js` | 시험용 가짜 백엔드. A-D JSON v6 형식, 10Hz, 승객 1명이 좌우로 움직임 |
| `.env` | 키와 포트. **커밋 금지** (`.gitignore` 처리) |

## 1. 내 노트북에서 실행 (서버 없이 시험)

필요한 것: Docker Desktop, Node.js 20

`deploy` 폴더에 `.env` 파일을 만듭니다. 내 노트북에서만 쓰는 키라 아무 문자열이나 괜찮습니다.

```text
INGEST_KEY=local-test-key
```

```bash
cd deploy
docker compose up -d --build
```

브라우저에서 `http://localhost:18080` → ElevSafe 화면이 뜨면 성공입니다. (아직 데이터가 없어서 승객은 안 보임)

포트를 바꾸려면 `.env`에 `WEB_PORT=다른번호`를 추가합니다. 기본값 18080은 프론트 개발 서버(3000)와 겹치지 않게 정한 값입니다.

## 2. 가짜 데이터로 시험

터미널 두 개를 씁니다.

**터미널 1: 가짜 백엔드**

```bash
cd deploy/bridge
npm install
node fake_backend.js
```

→ `[fake] ws://127.0.0.1:18081/radar`

**터미널 2: bridge**

macOS / Linux

```bash
cd deploy/bridge
LOCAL_URL=ws://127.0.0.1:18081/radar REMOTE_URL="ws://127.0.0.1:18080/ingest?key=local-test-key" node bridge.js
```

Windows (PowerShell)

```powershell
cd deploy\bridge
$env:LOCAL_URL="ws://127.0.0.1:18081/radar"; $env:REMOTE_URL="ws://127.0.0.1:18080/ingest?key=local-test-key"; node bridge.js
```

→ `[bridge] 서버 연결됨`, `[bridge] 로컬 백엔드 연결됨`

**기대값:** `http://localhost:18080`에서 `TEST #1` 점이 캐빈 안에서 좌우로 움직입니다(약 12초에 한 번 왕복).

| 시험 | 방법 | 기대값 |
|---|---|---|
| 보기 전용 | 화면에서 `강제 OPEN` 클릭 | 도어 상태 `CLOSED` 그대로 |
| 서버 끊김 | 터미널 2에서 `Ctrl+C` | 화면 점만 멈추고, 터미널 1은 계속 동작 |
| 재연결 | 터미널 2 다시 실행 | 점이 다시 움직임 |
| 백엔드 재시작 | 터미널 1을 껐다 켬 | bridge가 2초마다 재연결 시도 후 다시 붙음 |
| 키 오류 | `key=`를 틀리게 실행 | `docker compose logs relay`에 `키가 틀린 노트북 연결 거부` |

## 3. 실제 백엔드로 시험 (A)

백엔드를 Mock 또는 실제 센서로 실행하면 기본 주소가 `ws://127.0.0.1:8080/radar`이고, bridge의 `LOCAL_URL` 기본값과 같습니다.

```bash
./build-integrated/backend/elevsafe_backend --ws-mock --ws-duration 1800
```

```bash
cd deploy/bridge
REMOTE_URL="ws://127.0.0.1:18080/ingest?key=local-test-key" node bridge.js
```

## 4. 프론트 개발 서버로 시험 (D)

`npm start`일 때 `App.jsx`의 WebSocket 주소 기본값은 `ws://127.0.0.1:8080/radar`(백엔드 직접 연결)입니다. 로컬 relay를 보려면 주소를 지정합니다.

```bash
cd frontend
REACT_APP_WS_URL=ws://127.0.0.1:18080/radar npm start
```

빌드해서 서버에 올리면 따로 지정하지 않아도 "페이지를 연 주소의 `/radar`"로 자동 연결됩니다.

## 5. 학교 서버 반영

학교 서버는 김수연(B) 개인 수업 계정이라, **반영은 수연에게 요청**해 주세요. (PR 병합 후 디스코드로 알려 주시면 됩니다)

- 주소: `http://163.239.77.89:3000` (학교 밖 LTE에서도 접속 가능)
- 서버 `.env`에는 `WEB_PORT=3000`과 서버 전용 키가 있습니다.

반영 명령 (수연)

```bash
cd ~/ElevSafe
git pull
cd deploy
docker compose up -d --build
docker compose ps
```

서버에는 Node가 없어서, 서버 안에서 가짜 데이터로 시험할 때는 Docker로 실행합니다.

```bash
cd ~/ElevSafe/deploy
docker run --rm --init -it --name elevsafe-fake --network host -u "$(id -u):$(id -g)" -e npm_config_cache=/tmp/.npm -v "$PWD/bridge:/app" -w /app node:20-alpine sh -c "npm install && node fake_backend.js"
```

```bash
cd ~/ElevSafe/deploy
set -a; . ./.env; set +a
docker run --rm --init -it --name elevsafe-bridge --network host -u "$(id -u):$(id -g)" -e npm_config_cache=/tmp/.npm -e LOCAL_URL=ws://127.0.0.1:18081/radar -e REMOTE_URL="ws://127.0.0.1:${WEB_PORT}/ingest?key=$INGEST_KEY" -v "$PWD/bridge:/app" -w /app node:20-alpine sh -c "npm install && node bridge.js"
```

## 주의

- **키(`INGEST_KEY`)는 레포·디스코드·노션에 올리지 않습니다.**
- 학교 서버는 다른 수업 컨테이너(FastAPI, SonarQube, Grafana 등)와 같이 씁니다. `docker system prune`, `docker builder prune`, `--remove-orphans`처럼 **전체를 정리하는 명령은 쓰지 않습니다.** 정리는 `deploy` 폴더에서 `docker compose down`만 사용합니다.
- 학교 방화벽이 밖에서는 정해진 몇 개 포트만 열어 둡니다. 서버 포트를 바꿔야 하면 먼저 공유합니다.
- `ws` 버전은 보안 문제로 8.22.1 이상을 씁니다. 바꿀 때는 `relay`와 `bridge`의 `package.json`을 함께 바꿉니다.

## 남은 일 (11/20 데모 이후)

| 할 일 | 담당 |
|---|---|
| 백엔드에서 서버로 직접 보내는 연결 (bridge 대체), 서버 끊김 시 현장 처리 유지 | A |
| 백엔드 WebSocket 서버 `reuse_address` (Linux 재시작 시 포트 사용 중 오류 방지) | A |
| 원격 화면에서 문 제어 버튼 숨기기, 좌표 표시 소수 자리 줄이기 | D |
| relay: `init: true`(종료 지연 해결), 노트북 연결 1개만 허용, 느린 화면 프레임 건너뛰기, 메시지 크기 제한 | A 또는 B |
