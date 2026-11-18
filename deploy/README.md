# 학교 서버 중계 배포

현장 노트북의 A → B → C 결과를 Bridge와 Relay로 원격 D 화면에 전달한다.
현장 Backend는 학교 서버에 직접 접속하지 않는다. C SafetyJudge와 물리 문 제어는 포함하지 않는다.

```text
Radar Emulator / 센서 → A Parser → B SignalPipeline → C TrackingEngine
  → A WebSocket → 노트북 Bridge → nginx /ingest → Relay → nginx /radar → D 화면
```

최신 main의 Frontend를 그대로 사용한다. UNKNOWN 표시, 재접속 backoff, 렌더링,
개발/배포 WebSocket URL 분기는 변경하지 않는다. 원격 문 버튼은 PR #33 리뷰 합의대로 남긴다.
Relay는 viewer의 모든 메시지를 버리며 Bridge에도 원격 → Backend 전달 경로가 없다.
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
# INGEST_KEY가 이미 안전하게 설정된 상태
# 기본 웹 포트 18080. 기존 프로세스/포트 점유를 먼저 확인한다.
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

브라우저에서 `http://localhost:18080`을 연다. 실제 A를 사용할 때는 LOCAL_URL을
`ws://127.0.0.1:8080/radar`로 바꾸거나 생략한다. 최신 main을 빌드한 Backend에서:

```powershell
& $BackendExe --ws-mock --ws-bind 127.0.0.1 --ws-port 8080 --ws-duration 600
# 또는 Repository 루트에서 실제 PCR/Emulator 입력:
& $BackendExe 127.0.0.1 29172 --signal-config .\signal_processing\config\elevator_2nd.cfg `
  --websocket --ws-bind 127.0.0.1 --ws-port 8080
```

`$BackendExe`는 실제 Release 경로를 확인해 지정한다. PCR과 배경맵은 별도 사용자 데이터다.
실재생에서는 시작 로그의 `background=enabled`를 확인한다. SafetyJudge는 연결하지 않는다.

D 개발 실행은 기본적으로 로컬 A에 연결한다. 로컬 Relay를 확인하려면:

```powershell
cd frontend
$env:REACT_APP_WS_URL = 'ws://127.0.0.1:18080/radar'
npm start
```

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
- 이 구성 자체는 TLS를 설치하지 않는다. 공개 네트워크에서는 wss 또는 승인된 TLS proxy/VPN을 준비한다.
  ws로 키와 Snapshot을 보내면 평문이다. viewer 인증도 아직 없다.
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
