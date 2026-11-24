# 학교 서버 중계 배포

현장 노트북의 A → B → C 결과를 Bridge와 Relay로 원격 D 화면에 전달한다.
현장 Backend는 학교 서버에 직접 접속하지 않는다. C SafetyJudge와 물리 문 제어는 포함하지 않는다.

```text
Radar Emulator / 센서 → A Parser → B SignalPipeline → C TrackingEngine
  → A WebSocket → 노트북 Bridge → nginx /ingest → Relay → nginx /radar → D 화면
```

최신 main의 Frontend를 그대로 사용한다. UNKNOWN 표시, 재접속 backoff, 렌더링,
개발/배포 WebSocket URL 분기는 변경하지 않는다. 원격 문 버튼은 PR #33 리뷰 합의대로 남긴다.
HTTP는 로그인 없이 접속하며 HTTPS는 nginx Basic 인증을 요구한다. 제어를 켜면 같은 `/radar` 연결에서 가상 문 명령을 접수한다.
화면의 명령 송신 표시가 실제 명령 접수·실행·ACK를 의미하지 않는다.

## 구성과 실행

- `bridge/bridge.js`: 로컬 WebSocket을 구독하고 원격에 현재 Snapshot만 전달한다.
- `relay/relay.js`: 한 ingest의 Snapshot을 여러 viewer에게 전달한다.
- `relay/transport.js`: 크기·버퍼 제한, v7 입력 검사와 종료 보조 코드. Bridge도 이 파일을 사용하므로 `deploy/` 구조를 함께 유지한다.
- `bridge/fake_backend.js`: 100ms 간격의 합성 입력. 실제 B/C 판단이 아니다.
- `web/`: 최신 Frontend 빌드와 nginx WebSocket proxy.
- `tests/transport.test.js`: 로컬 자동 회귀 시험.

로컬 실행에는 Node.js 20 이상, 컨테이너 배포에는 Docker Compose가 필요하다.
Bridge와 Relay는 기존 고정 버전 `ws` 8.22.1을 사용한다.

```powershell
# Repository 루트에서 (운영 서버 실행 명령이 아님)
npm install --prefix deploy/relay --ignore-scripts
npm install --prefix deploy/bridge --ignore-scripts
node --test --test-concurrency=1 deploy/tests/transport.test.js
```

키는 승인된 비밀 전달 수단으로 `INGEST_KEY` 환경변수 또는 Git 제외 `deploy/.env`에 공급한다.
키와 키를 포함한 REMOTE_URL을 터미널 출력·명령 이력·문서·로그에 붙여 넣지 않는다.
이 문서에는 실제 키를 기록하지 않는다.

별도 로컬 시험 환경에서만:

```powershell
cd deploy
# 아래의 INGEST_KEY, PROXY_TOKEN_FILE을 준비한다. HTTP 기본 주소는 http://localhost:18080이다.
docker compose up -d --build
docker compose ps
```

별도 터미널에서:

```powershell
cd deploy/bridge
node fake_backend.js
# Fake Backend: ws://127.0.0.1:18081/radar
```

Bridge 터미널은 `REMOTE_URL`이 승인된 방식으로 이미 설정돼 있어야 한다.
형식은 `ws[s]://<호스트>:<포트>/ingest?key=<비밀값>`이다.

```powershell
cd deploy/bridge
$env:LOCAL_URL = 'ws://127.0.0.1:18081/radar'
node bridge.js
```

브라우저에서 `CONTROL_HTTP_ORIGIN` 또는 `CONTROL_ORIGIN`으로 지정한 주소를 연다. 실제 A를 사용할 때는 LOCAL_URL을
`ws://127.0.0.1:8080/radar`로 바꾸거나 생략한다. 최신 main을 빌드한 Backend에서:

```powershell
& $BackendExe --ws-mock --ws-bind 127.0.0.1 --ws-port 8080 --ws-duration 600
# 또는 Repository 루트에서 실제 PCR/Emulator 입력:
& $BackendExe 127.0.0.1 29172 --signal-config .\signal_processing\config\elevator_2nd.cfg `
  --websocket --ws-bind 127.0.0.1 --ws-port 8080
```

`$BackendExe`는 실제 Release 경로를 확인해 지정한다. PCR과 배경맵은 별도 사용자 데이터다.
실재생에서는 시작 로그의 `background=enabled`를 확인한다. SafetyJudge는 연결하지 않는다.

D 개발 실행(`frontend`에서 `npm start`)은 기본적으로 로컬 A의 `/radar`에 직접 연결한다.

배포 빌드는 기본적으로 페이지와 같은 호스트의 `/radar`를 사용한다. HTTPS 페이지는 wss를 쓴다.
REACT_APP_WS_URL은 빌드 시 설정이며 컨테이너 실행 후 바꿔도 이미 생성된 JS에는 반영되지 않는다.

Linux 컨테이너에서 Bridge를 실행할 때는 전체 deploy를 마운트한다.
bridge 디렉터리만 마운트하면 ../relay/transport.js를 찾지 못한다.
REMOTE_URL은 안전하게 미리 설정한다. 아래는 별도 로컬 Linux 시험용 예시다.

```bash
cd deploy
docker run --rm --init -it --network host \
  -u "$(id -u):$(id -g)" -e npm_config_cache=/tmp/.npm \
  -e LOCAL_URL -e REMOTE_URL \
  -v "$PWD:/app" -w /app/bridge node:20-alpine \
  sh -c "npm install --ignore-scripts && node bridge.js"
```

Linux host 네트워크 기준이며 Docker Desktop 및 운영 서버 설정은 별도 확인한다.
Ctrl+C로 종료한다. 운영 서버 실행은 서버 담당자 승인 후 수행한다.

## Windows에서 Backend와 Bridge 함께 실행

Windows PowerShell 5.1, Node.js 20 이상, 사용할 Release Backend EXE가 필요하다.
저장소 루트에서 `npm install --prefix deploy/bridge --ignore-scripts`로 Bridge 의존성을 준비한다.
스크립트는 EXE를 빌드하거나 의존성을 설치하지 않는다. 시작 시 EXE의 SHA256을 출력한다.
사용할 빌드의 해시를 알고 있다면 `-ExpectedBackendSha256`으로 일치 여부를 검사할 수 있다.

`REMOTE_URL`은 승인된 비밀 전달 방식으로 현재 PowerShell 환경에 미리 공급한다.
형식은 `wss://<relay-host>/ingest?key=<비밀값>`이다. 실제 키를 명령 이력이나 로그에 남기지 않는다.
격리 HTTP 시연에서는 `ws://`를 사용할 수 있다. 서버 제어 스위치의 기본값은 OFF다.

아래 `$BackendExe`는 저장소 루트에서 `build`에 구성한 Windows x64 Release 빌드의 예시다.
별도 출력 폴더를 사용했다면 실제 EXE 경로를 지정한다.

```powershell
$BackendExe = '.\build\backend\Release\elevsafe_backend.exe'
.\deploy\start-backend-bridge.ps1 -Mode Mock -BackendExe $BackendExe -MockDuration 600
```

Mock에서 소프트웨어 문 시뮬레이션이 필요하면 `-DoorSim`을 추가한다.
Radar 모드는 Emulator를 먼저 시작한 후 실행한다. 실센서에서는 `-RadarHost`와 `-RadarPort`를 실제 주소와 포트로 바꾼다.

```powershell
.\deploy\start-backend-bridge.ps1 -Mode Radar -BackendExe $BackendExe -RadarHost 127.0.0.1 -RadarPort 29172 -SignalConfig .\signal_processing\config\elevator_2nd.cfg
```

상대 EXE·설정 경로는 스크립트가 속한 저장소 루트 기준이다. 공백이 있는 절대 경로는 따옴표로 감싼다.
WebSocket은 loopback에 바인딩하며 `-WsPort` 기본값은 8080, `-ReadyTimeoutSeconds` 기본값은 15초다.

- `Local WebSocket ready` 후 Bridge가 시작된다. Bridge의 `local connected`와 `remote connected`는 각각의 핸드셰이크 결과다. 실제 Snapshot은 Viewer에서 확인한다.
- 원격 Relay가 연결되지 않으면 Bridge가 기존 정책으로 재접속하며 Backend는 계속 동작한다.
- Ctrl+C 또는 자식 종료 시 이 스크립트가 시작한 프로세스만 정리한다. 종료가 지연되면 해당 프로세스만 제한 시간 후 종료한다. Mock 시간이 정상적으로 끝나면 Bridge도 정리한다.
- 시작 오류가 나면 출력에 표시된 EXE·설정·Node 모듈·`REMOTE_URL`을 확인한다. 포트가 이미 사용 중이면 다른 `-WsPort`를 지정하거나 해당 포트의 소유자에게 확인한다.


## 제한값과 실패 처리

| 설정 | 기본값 | 적용 및 근거 |
|---|---:|---|
| MAX_MESSAGE_BYTES | 65536 bytes | A의 maxSnapshotBytes 64KiB와 동일. Bridge/Relay 모두 적용 |
| MAX_BUFFERED_BYTES | 131072 bytes | 최대 Snapshot 2개 분량. bufferedAmount + 새 메시지 크기로 판단 |
| STALE_MS | 3000ms | Relay 캐시 만료. 목표 10Hz 기준 약 30주기의 입력 공백; 운영 값은 팀 합의 후 조정 |
| RECONNECT_MS | 2000ms | Bridge의 기존 close 후 재접속 주기 |
| CONNECT_TIMEOUT_MS | 5000ms | Bridge 연결 handshake 대기 상한 |
| HEARTBEAT_MS | 30000ms | ping/pong 감시. 한 주기 내 pong이 없으면 다음 검사에서 종료 |
| PORT | 8090 | Relay 내부 포트. nginx 설정도 함께 확인 |
| WEB_PORT | 18080 | Compose 호스트 포트. 운영 포트는 서버 담당자 지정 |

정수 설정은 유효 범위를 검사하고 버퍼 상한은 최대 메시지 한 개 이상이어야 한다.
LOCAL_URL/REMOTE_URL은 ws 또는 wss URL이어야 하며 URL 사용자명·비밀번호·fragment는 거부한다.
설정 오류 로그에는 URL이나 키를 출력하지 않는다.

- ingest 키가 틀리면 HTTP 403, 이미 ingest가 있으면 HTTP 409로 새 연결만 거부한다.
  기존 연결을 강제 교체하지 않는다. 오래된 연결의 종료 처리가 끝난 뒤 새 연결을 받는다.
- ingest 종료 시 캐시를 즉시 무효화한다. 유효 Snapshot 수신 시 Node monotonic 시각을 저장한다.
  만료는 주기 검사와 신규 viewer 접속 시 검사한다. `timestamp_us`를 현재 시각과 비교하지 않는다.
  Radar 새 세션이 timestamp_us=0으로 돌아와도 전달하며 세션 간 연속성을 가정하지 않는다.
- 잘못된 JSON·필드·binary 메시지는 전달하지 않고 캐시 시각도 갱신하지 않는다.
  WebSocket 크기 초과는 해당 연결을 종료한다. 메시지 원문과 키는 로그에 출력하지 않는다.
- JSON v7 필드를 검사하되 전달 문자열은 재직렬화하지 않는다. UNKNOWN, null, 원본 숫자 문자열을 유지한다.
- 버퍼 상한을 넘는 remote/viewer는 종료해 적체된 오래된 Snapshot을 버린다.
  Bridge는 원격 복구 후 다음 로컬 Snapshot부터 전달하며 과거 데이터를 재전송하지 않는다.
  원격 장애 중에도 로컬 구독은 계속하고 데이터만 버린다.
- 정상 종료 시 재접속·heartbeat·만료 타이머를 해제하고 연결을 닫는다.
  500ms 안에 닫히지 않는 소켓은 종료한다. Relay의 SIGINT/SIGTERM 처리와 Compose init을 사용한다.

**캐시 만료는 기존 화면을 자동으로 지우지 않는다.** D는 마지막 데이터를 계속 표시할 수 있다.
연결 상태/마지막 유효 수신 시각/stale 표시는 D와 협의할 후속 범위다. stale를 나타내려고
`door_state`, `door_command`를 바꾸거나 새로운 JSON 필드를 보내지 않는다.

## 서버 보안과 배포 범위

학교 서버 배포는 서버 담당자 승인 후 담당자가 수행한다. 이번 작업은 운영 서버에 접속하지 않는다.
기존 PR 기록의 학교 주소/포트는 현재 운영 상태를 보증하지 않는다.

- nginx의 /ingest와 /radar access log는 끈다. 상위 proxy·error log·모니터링의 URL 수집도 서버 담당자가 점검해야 한다.
- nginx는 주입된 인증서로 TLS를 종료한다. 운영 도메인과 인증서 발급·갱신은 서버 담당자가 준비한다.
  HTTPS/WSS를 구성하면 Dashboard와 /radar는 동일한 realm에서 인증한다.
- Compose 이름은 기존 `elevsafe`, relay 256MiB/0.5CPU, web 128MiB/0.5CPU, 로그 10MiB×3을 유지한다.
  동일 프로젝트/포트 중복 실행과 다른 컨테이너 영향은 서버 담당자가 확인한다.
- 서버 전체 prune, 무관한 컨테이너 종료, 임의 포트 변경을 하지 않는다.
- Backend 직접 원격 송신과 소켓 reuse 옵션 변경은 이번 작업에 필요하지 않다.

## 검증 범위

최신 main `2a2a62639d30024d8dba6019006a55089ed5e6e7` 이식 후 검증 (2026-11-17).
동일 제품 코드와 assertion 및 ws 8.22.1을 사용했다. 기존 10개 시험을 유지하고 JSON v7
문 상태·명령·승객 필드 조합과 잘못된 입력 검증을 보강했다.

| 환경 | 전체 transport 시험 | 독립 합성 E2E | ExitCode |
|---|---|---|---|
| Windows Node 26.13.0 | 10/10 PASS | 1/1 PASS | 각각 0 |
| WSL Ubuntu Node 20.20.2 | 10/10 PASS | 1/1 PASS | 각각 0 |
| WSL Ubuntu Node 24.21.0 | 10/10 PASS | 1/1 PASS | 각각 0 |

- E2E: 기본 TTL 3000ms, 두 viewer 원문 일치, 만료 후 신규 viewer 미전송, 인증/추가 ingest 거부,
  Backend 재접속, Bridge 종료 후 Fake Backend 독립성 확인.
  Relay 재시작, 느린 viewer/remote, 큰 메시지 처리는 전체 transport 시험에서 확인했다.
- 각 전체 시험의 fixture cleanup 34/34 완료. 이번 EFAULT 미재현은 근본 원인 해결을 뜻하지 않는다.
- Windows MSVC x64 루트 Release Build PASS. 등록된 Backend/C Tracking CTest는
  **22/23 PASS, ExitCode 8**. `radar_reconnect_cli_test`의 `refused-limit`에서
  `backend.exit` 12초 한도를 넘었다. 실패 로그를 보존했고 재시도하지 않았다.
  당시 다른 빌드 작업도 진행 중이었으나 이를 원인으로 확정하지 않는다.
- B 독립 Release Build 및 `sp_tests` 1/1 PASS. 기존 main 설정상 `tracking/S0-1_t01.csv`가 없으면
  detection_loader_test를 등록하지 않는다. 해당 시험은 미수행이며 등록 설정은 변경하지 않았다.
- Frontend CI=true Build FAIL: 기존 App.jsx:294의 receiveMessage Hook 의존성 경고.
  D 소스는 변경하지 않았다. 기존 Dockerfile과 같은 CI=false 배포 Build는 경고를 남기고 PASS (ExitCode 0).
- Node 문법, 모듈 경로, 환경설정 오류의 종료 코드와 비밀값 비노출, Compose YAML·자원·포트 정적 검사 PASS.
  Docker/Podman/nginx 도구 부재로 docker compose config, nginx -t, Alpine 빌드/실행은 미수행.
- 로그는 새 Worktree의 deploy/diagnostics 및 Linux 격리 사본에 보존했다.
  Windows npm 오프라인 캐시 조회 실패 후 기존 ws 8.22.1 설치본을 격리 복사했다.
  Linux는 캐시를 사용했다. 기존 시험 및 실패 로그는 변경하지 않았다.

이전 안정화 Worktree의 CTest 23/23 PASS는 이번 결과를 대체하지 않는다.
FOV_t03 PCR → A/B/C → D 표시, Bridge 중단 후 처리 지속, 신규 viewer stale 차단과 복구는
이전 사용자 수동 검증 결과다. 이번 합성 E2E를 PCR/D 화면 재검증으로 해석하지 않는다.
학교 서버·LTE·Alpine·운영 TLS, 센서 전체 송신 프레임 보존, D 기존 화면 stale 표시는 미검증이다.
C SafetyJudge 및 물리 문 제어는 이번 구현에 포함하지 않는다.

## PR #41 병합 후 통합 검증 (2026-11-18)

기준 main은 `2a3757a80650686a7f39837db9e47c51fe2ada89`이다.
기존 안정화 Worktree를 보존하고 별도 Worktree에서 deploy 9개 파일만 반영했다.
PR #40의 CI 캐시와 PR #41의 B 원점·ROI 변경은 그대로 유지했다.
deploy 파일과 수정 영역이 겹치지 않아 파일 충돌은 없었다.
Bridge/Relay는 좌표 원문을 전달하며, 새 원점 기준의 PCR/D 화면 검증은 별도로 필요하다.

| 환경 | transport | 합성 E2E | 종료 코드 |
|---|---|---|---|
| Windows Node 26.13.0 | 9/10 PASS | 1/1 PASS | 각각 1 / 0 |
| WSL Node 20.20.2 | 10/10 PASS | 1/1 PASS | 각각 0 |
| WSL Node 24.21.0 | 10/10 PASS | 1/1 PASS | 각각 0 |

Windows 실패는 큰 메시지 시험의 Relay 준비 단계 listen EFAULT (errno -4074, 요청 127.0.0.1:0)다.
메시지 처리 assertion에 도달하기 전 실패했고 cleanup 31/31 완료했다. 재시도하지 않았다.
WSL 두 환경의 cleanup은 각각 34/34 완료했다. Linux 성공으로 Windows 실패를 대체하지 않는다.

과거 실패는 별도 기록으로 유지한다: 최초 radar_reconnect_cli_test의 backend.exit 12초 시간 초과와,
이후 격리 전체 시험 websocket_cli_test의 radar-profile / fixture.listen Winsock 10014는 서로 다른 실행이다.
현재 시험으로 과거 실패가 해결됐다고 주장하지 않는다.

이번 통합 상태의 Windows MSVC x64 루트 Release 및 B 독립 Build는 모두 PASS (ExitCode 0).
모든 빌드 종료 후 CTest를 -j 1로 각각 한 번 실행했다.
- 루트 CTest: 23/23 PASS, 56.43초, ExitCode 0.
- B 독립 CTest: sp_tests 1/1 PASS, 0.34초, ExitCode 0.
- 이번 radar_reconnect_cli_test 및 websocket_cli_test는 PASS. 과거 실패 원인 해결을 뜻하지 않는다.
- Frontend CI=true Build는 기존 App.jsx:294 Hook 의존성 경고로 FAIL (ExitCode 1).
  기존 Dockerfile 조건 CI=false Build는 같은 경고를 유지하며 PASS (ExitCode 0).
- detection_loader_test는 기존 main의 외부 CSV 부재 정책에 따라 미등록이며, 이번에 제외하지 않았다.

새 결과는 검증 Worktree의 deploy/diagnostics에 별도로 보존했다.
학교 서버, LTE, Alpine, 최신 B 원점을 적용한 PCR/D 화면은 이번에도 미검증이다.
Frontend/Backend/B/C 제품 소스를 별도 수정하지 않았다.
이번 Windows Node 9/10 실패와 과거 CTest 실패를 PR에 명시해야 한다.



## 접속 프로토콜별 Dashboard와 가상 문 관제

HTTP와 HTTPS는 같은 Dashboard 정적 파일과 `/radar`를 사용한다. D 소스 변경이나 별도 조회 화면은 필요 없다. 개발 환경의 `npm start`는 기존처럼 현장 Backend로 직접 연결한다.

| 접속 | Dashboard 및 Snapshot | 가상 문 버튼이 명령을 전달하는 조건 |
|---|---|---|
| HTTP / WS | 무인증 | Relay의 `CONTROL_ENABLED=1` 및 `CONTROL_ALLOW_INSECURE_DEMO=1` |
| HTTPS / WSS | 문서·정적 파일·WebSocket 모두 Basic 인증 | 인증 성공 및 `CONTROL_ENABLED=1` |

두 스위치는 기본 OFF다. HTTP 관제를 열면 해당 주소에 도달하는 누구나 가상 문을 조작할 수 있다. HTTPS의 비밀번호는 HTTP 경로를 보호하지 않는다. HTTP에서는 서비스 자격증명도 전송 중 노출될 수 있으므로 접근을 제한한 시연망에서만 사용한다. 실제 엘리베이터 및 STM32는 대상에서 제외한다.

### 설치 전 준비

| Compose 설정 | 공급 위치 및 용도 |
|---|---|
| `CONTROL_HTTP_ORIGIN` | 실제 HTTP 주소와 포트. 기본 `http://localhost:18080`. 포트나 호스트를 바꾸면 함께 설정 |
| `CONTROL_ORIGIN` | 정확한 외부 HTTPS origin 하나. 비표준 포트는 포함하고 경로와 끝 슬래시는 생략한다. |
| `DASHBOARD_USERS_FILE` | 저장소 밖에 생성한 bcrypt htpasswd 파일 경로. nginx에만 읽기 전용 제공 |
| `TLS_CHAIN_FILE`, `TLS_KEY_FILE` | 해당 도메인의 인증서 체인과 개인키 경로. nginx에만 제공 |
| `PROXY_TOKEN_FILE` | nginx와 Relay만 읽는 별도 비밀 파일. 32~256자의 무작위 base64url 문자열 한 줄 |
| `INGEST_KEY` | 기존 단일 생산자 `/ingest` 인증 |
| `CONTROL_ENABLED=1`, `CONTROL_BRIDGE_TOKEN` | 원격 명령을 허용할 때 Relay와 Bridge에 설정. Bridge는 `/control-link`에 전용 Bearer 인증 |
| `CONTROL_LOCAL_TOKEN` | Bridge와 loopback A만 공유. A의 `--ws-remote-control`은 Mock 문 시뮬레이션 또는 Radar Emulator 문 시뮬레이션에서만 허용 |

예를 들어 제한된 디렉터리에서 `umask 077` 후 `htpasswd -cB /private/elevsafe/users admin`을 실행하고 프롬프트에서 시험 비밀번호를 입력한다. 기존 계정 파일을 갱신할 때는 `-c`를 빼고, 운영에는 별도의 긴 임의 비밀번호를 사용한다. `openssl rand -hex 32 > /private/elevsafe/proxy-token`으로 프록시 전용 비밀을 생성할 수 있다. 모든 기계용 키는 서로 다르게 생성한다. Compose secret의 원본 파일은 필요한 컨테이너 프로세스가 읽을 수 있어야 한다. 저장소·이미지·프론트 빌드에 이 파일을 넣지 않는다.

기본 `docker compose up -d --build`는 HTTP만 기동한다. 인증서·개인키·htpasswd 파일은 이 구성에 필요하지 않다. `WEB_BIND`와 `WEB_PORT`는 호스트 바인딩이며 기본은 `127.0.0.1:18080`이다. 다른 기기에서 사용할 때는 승인된 시연망 인터페이스와 외부 Origin을 지정한다. 예를 들어 포트 3000을 사용하면 `WEB_PORT=3000`, `CONTROL_HTTP_ORIGIN=http://<시연호스트>:3000`으로 맞춘다.

HTTPS를 함께 제공하려면 TLS·계정 파일과 `CONTROL_ORIGIN`을 설정하고 아래 명령을 사용한다. HTTPS 기본 바인딩은 `127.0.0.1:18443`이며 `HTTPS_BIND`, `HTTPS_PORT`로 지정한다. HTTP 관제를 끄려면 두 서비스에서 `CONTROL_ALLOW_INSECURE_DEMO=0`으로 재기동하고 Bridge의 제어 연결은 WSS로 전환한다. HTTP 조회까지 외부에 제공하지 않을 경우 HTTP 리스너의 호스트 바인딩을 loopback에 두고 상위 프록시도 해당 포트를 공개하지 않는다.

```sh
docker compose -f docker-compose.yml -f docker-compose.https.yml up -d --build
```

Relay에는 공개 포트가 없다. nginx는 `/radar`에서 브라우저 Authorization을 제거하고, 실제 리스너의 scheme·인증 결과·프록시 전용 자격증명을 덮어쓴다. Relay는 이 증명과 해당 scheme의 정확한 Origin을 검사한다. `/ingest` 키, `/control-link` Bearer, loopback `/control-local` 인증은 HTTP 시연에서도 필요하다. HTTP 전용이면 Bridge에도 `CONTROL_ALLOW_INSECURE_DEMO=1`을 설정하고 `CONTROL_REMOTE_URL=ws://<시연호스트>:<포트>/control-link`를 사용한다. HTTPS에서는 `wss://`를 사용한다. Backend/Bridge 실행 시 Mock은 `-DoorSim -RemoteControl`, Radar Emulator는 `-EmulatorDoorSim -RemoteControl`을 사용한다.

상위 프록시가 TLS를 종료하면 인증을 적용하는 내부 nginx의 **HTTPS 리스너로 다시 연결**하고 upstream 인증서를 검증한다. Host·Origin·Upgrade·Basic 헤더를 보존한다. HTTPS 요청을 내부 HTTP 시연 리스너에 전달하면 Basic 인증 경계가 사라진다. 단순히 클라이언트 전달 헤더로 HTTPS를 판정하는 구성은 지원하지 않는다.

### 명령과 소유권

제어를 허용한 소켓마다 독립 운영자 ID를 발급한다. 입력은 단일 필드 `{"door_command":"OPEN"}` 형식이며 HOLD/CLOSE도 같은 형식이다. Relay가 요청 ID와 TTL을 부여한다. 최대 접속은 16개, 제어 입력 기본 상한은 1KiB, 빈도는 연결당 초당 5건(`CONTROL_RATE`), 전체는 그 네 배다. HTTP 및 HTTPS 전체에서 미완료 요청은 한 건이다. 처리 중 새 요청은 QUEUE_FULL로 거부되고 재전송되지 않는다. 반복 클릭에는 별도의 요청 ID가 부여된다.

A는 명령 적용 직전에 세션과 최초 발급 TTL을 확인한다. 기본 TTL은 2초이며 Relay·Bridge·A의 `CONTROL_TTL_MS`를 맞춘다. 원격 → 현장 수동 → C 안전 → A 정상 운행 순서를 유지한다. 원격 HOLD 소유자가 끊어지면 문은 HOLD를 유지하고 현장 제어권이 복구된다. 소유권을 넘겨받은 뒤 이전 연결이 끊겨도 현재 소유권은 유지된다.

D는 송신 직후 최근 명령을 표시한다. 접수·실제 적용·만료는 Relay의 `[control]` 진단에서 요청 ID와 결과로 구분하고, 실제 문 상태는 기존 Snapshot에서 확인한다. `/radar`에는 JSON v7 Snapshot만 전송한다. 공용 계정으로는 개인을 식별할 수 없으며 브라우저 Basic 인증 캐시는 별도 React 로그아웃을 제공하지 않는다.

### 계정 변경과 연결 폐기

변경 시간을 정하고 web을 중지해 기존 브라우저와 Bridge 연결을 닫는다. Relay를 중지하고 현장에서 미적용 요청 폐기·제어권 복구를 확인한다. 안전한 계정 파일을 교체한 뒤 Relay와 web을 재생성해 secret 마운트를 갱신한다. nginx의 단순 reload 또는 비밀번호 파일 변경만으로 열린 WS가 모두 폐기되지는 않는다. 새 비밀번호로 재인증하고 이전 비밀번호 거부, 기존 HOLD 유지, 새 명령만 적용되는지 확인한다.

서버 인증 경계의 실제 시험에는 `ELEVSAFE_TEST_NGINX`(nginx 실행 파일), `ELEVSAFE_TEST_HTPASSWD`, `ELEVSAFE_TEST_PASSWORD`를 격리된 Linux 환경에 지정한다. `ELEVSAFE_TEST_BACKEND`를 설정하면 가상 문 통합 시험을, `ELEVSAFE_TEST_FOV_EMULATOR`, `ELEVSAFE_TEST_FOV_DATA`, `ELEVSAFE_TEST_FOV_CONFIG`까지 설정하면 실제 FOV 재생 시험을 등록한다. 시험은 임시 loopback HTTP 및 HTTPS 포트를 사용한다. 실제 관제 화면용 빌드는 `ELEVSAFE_TEST_DASHBOARD`로 지정할 수 있다.
