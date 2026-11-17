# ElevSafe Backend

ElevSafe Backend는 Retina-4SN 또는 Radar Emulator에서 TCP raw binary
stream을 수신하고, packet/frame 경계를 복원하여 `RadarFrame`으로 변환하는
Radar interface와 초기 처리 파이프라인을 담당합니다.

현재 Backend는 수신한 frame을 `BackendPipeline`과 `FrameBuffer`를 거쳐
consumer로 전달하는 단일 스레드 runtime입니다. `--signal-config PATH`를
지정하면 Adapter를 통해 B `SignalPipeline`을 호출하고 결과를 console에 출력합니다.
옵션을 생략하면 기존 수신/console 경로를 유지합니다. Radar 모드에서 `--signal-config`와
`--websocket`을 함께 지정하면 아래 시연용 B→C→WebSocket 경로를 활성화합니다.
C Safety/State 판단과 물리 문 제어는 연결하지 않습니다.

기존 WebSocket transport와 명시적 Mock 모드가 A-B 실행 경로와 공존합니다.
기존 WebSocket transport를 재사용하여 B 문 추정 상태와 C CONFIRMED Track을 전송합니다.
수동 FIFO와 Door Mock은 유지하며 Radar 결과로 물리 문 제어를 실행하지 않습니다.

## 디렉터리 구조

```text
backend/
├─ CMakeLists.txt
├─ README.md
├─ cmake/WebSocketDependencies.cmake
├─ include/
│  └─ elevsafe/
│     ├─ radar_types.h
│     ├─ relative_frame_clock.h
│     ├─ retina_protocol.h
│     ├─ retina_client.h
│     ├─ frame_buffer.h
│     ├─ backend_pipeline.h
│     ├─ signal_adapter.h
│     ├─ websocket_server.h
│     └─ websocket_message.h
├─ src/
│  ├─ main.cpp
│  ├─ retina_protocol.cpp
│  ├─ retina_client.cpp
│  ├─ frame_buffer.cpp
│  ├─ backend_pipeline.cpp
│  ├─ signal_adapter.cpp
│  ├─ websocket_server.cpp
│  └─ websocket_message.cpp
└─ tests/
   ├─ retina_protocol_test.cpp
   ├─ frame_buffer_test.cpp
   ├─ backend_pipeline_test.cpp
   ├─ relative_frame_clock_test.cpp
   ├─ signal_adapter_test.cpp
   ├─ signal_integration_test.cpp
   ├─ signal_replay_compare.cpp
   ├─ websocket_message_test.cpp
   ├─ websocket_server_test.cpp
   ├─ websocket_cli_test.cmake
   ├─ repeat_websocket_tests.ps1
   └─ data/                     # 독립적인 통합 테스트용 설정
```

## Runtime Data Flow

```text
RetinaClient
    -> RetinaStreamParser
    -> RadarFrame
    -> BackendPipeline::onFrame()
    -> FrameBuffer
    -> BackendPipeline::drain()
    -> Console Consumer
```

`--signal-config` 사용 시 drain consumer는 다음 경로도 실행합니다.

```text
RadarFrame -> toSignalFrame() -> elevsafe::sp::FrameData
    -> SignalPipeline::process(frame, FrameContext{}) -> PipelineResult -> Console
```

현재 `RetinaClient::run()`은 blocking receive loop입니다. 수신된 byte는
`RetinaStreamParser`로 전달되고, 완성된 `RadarFrame` callback은 같은
실행 context에서 동기적으로 호출됩니다. `main.cpp`의 callback은 frame을
`BackendPipeline::onFrame()`으로 enqueue한 뒤 즉시 `drain()`을 호출합니다.

### RadarPoint / RadarFrame

- `RadarPoint`는 point cloud 한 점의 `x`, `y`, `z`, `doppler`,
  `power`, `targetId`를 보관합니다.
- `RadarFrame`은 `frameCount`, `timestampUs`, `std::vector<RadarPoint>`를 보관합니다.
- `timestampUs`는 현재 TCP connection의 첫 valid frame을 `0`으로 한 host-side
  monotonic relative timestamp이며 단위는 microseconds입니다.

### RetinaStreamParser

TCP `recv()` 경계와 packet 경계가 일치하지 않는 상황을 처리합니다.
raw byte를 내부 stream buffer에 누적하고 packet magic과 `packageSize`를
사용해 완전한 network packet을 추출합니다. partial packet, 여러 packet이
하나의 receive에 포함된 경우, 앞부분 garbage에 대한 resynchronization을
처리합니다.

추출한 packet에서는 Radar Frame header와 point cloud를 검증하고
`RadarFrame`을 생성합니다. packet header는 36 bytes, Radar Frame header는
16 bytes이며, point count와 payload 범위도 검증합니다. 별도 target section의
상세 구조와 의미는 현재 구현 범위에 포함되지 않습니다.

### RetinaClient

지정된 host와 TCP port에 직접 연결합니다. 현재 기본 port는 `29172`이며,
Device Discovery는 사용하지 않습니다. 연결 후 raw byte를 수신하여
`RetinaStreamParser`에 전달하고, parser가 생성한 frame을 callback으로
전달합니다. 완성된 frame을 받은 직후 `std::chrono::steady_clock`을 사용해
connection-local `timestampUs`를 부여하며, 새 `run()`/connection에서는 첫
valid frame이 다시 `0`부터 시작합니다. 이 값은 sensor hardware timestamp가
아니며 `frameCount`나 고정 FPS로 보간하지 않습니다.

### FrameBuffer

`FrameBuffer`는 parsing이 완료된 `RadarFrame` 객체를 보관하는
`std::deque<RadarFrame>` 기반 bounded FIFO입니다.

- 생성 시 frame capacity를 지정합니다.
- capacity가 0이면 생성할 수 없습니다.
- 오래된 frame부터 소비하며, 남아 있는 frame의 FIFO 순서를 유지합니다.
- 용량이 가득 찬 상태에서 새 frame을 넣으면 가장 오래된 frame을 제거합니다.
- TCP raw byte를 보관하는 `RetinaStreamParser` 내부 buffer와는 별도입니다.

현재 `main.cpp`의 capacity `1`은 synchronous enqueue -> immediate drain
구조에 필요한 최소값입니다. production tuning 값으로 확정된 값이 아니며,
producer와 consumer가 분리되는 구조에서는 별도 검토가 필요합니다.

### BackendPipeline

`BackendPipeline`은 현재 Backend 처리 파이프라인의 skeleton입니다.

- `onFrame()`에서 입력 `RadarFrame`을 내부 `FrameBuffer`에 enqueue합니다.
- `drain()`에서 대기 중인 frame을 FIFO 순서로 consumer에 전달합니다.
- 현재는 generic `FrameConsumer` callback만 제공합니다.
- B 호출은 main의 consumer에서 수행하며 BackendPipeline 공개 API는 그대로입니다.
- B 결과의 C Tracking 연결은 main consumer의 시연 Adapter에서 수행합니다. C State/Safety 판단은 연결하지 않습니다.

### main.cpp

`main.cpp`는 host와 port를 읽고 `RetinaClient`를 실행합니다. callback에서
`onFrame()`을 호출한 직후 `drain()`을 호출하므로 현재 runtime은 별도
Radar 처리용 worker thread 없는 single-thread synchronous 구조입니다.
WebSocket을 활성화하면 transport만 별도의 I/O thread에서 동작합니다. 기존의
`frameCount`, `timestampUs`, `pointCount`, 첫 point의 `x/y/z/doppler/power/targetId`
출력은 `drain()`에 전달된 console consumer가 담당합니다.

### A-B Adapter 및 SignalPipeline

`toSignalFrame()`은 frame 번호, 정수 µs timestamp 및 point의
`x/y/z/doppler/power/targetId`를 B 타입에 복사합니다. 센서 원본 좌표·point 순서를
유지하며 zone, cluster, bins는 B 기본값을 사용합니다. 유효한 빈 frame도 전달합니다.
`frameCount`는 B의 `int`, `timestampUs`는 `int64_t`에 들어가는지 검사하며
범위를 넘으면 `std::overflow_error`로 해당 frame을 거부합니다. 원본은 변경하지 않습니다.

설정 로드 및 `SignalPipeline::ok()` 검사를 TCP 연결 전에 수행하며 실패하면 종료합니다.
연결 세션 동안 B 객체 하나를 유지하고 같은 thread에서 순차 호출합니다.
`FrameContext::door_state`는 항상 `UNKNOWN`이며 B 추정 결과를 되먹임하지 않습니다.
결과 로그에는 frame ID, timestamp, 입력 point 수, cluster 수와 추정 문 상태가 포함됩니다.

프레임 처리의 표준 예외는 frame 번호와 함께 stderr에 기록하고 해당 frame을 건너뜁니다.
이미 pop한 frame을 재시도하지 않으며 B 내부 상태의 rollback은 제공하지 않습니다.
메모리 할당 실패는 복구 가능한 frame 오류로 취급하지 않고 실행을 종료합니다.
B는 배경 맵 로드 실패 시 배경 제거를 생략하므로 시작 로그의 `background`를 확인하세요.
Live timestamp는 PCR 원본 상대 시간과 같은 기준 시각을 의미하지 않습니다.

## FrameBuffer 운영 정책

### 현 단계 잠정 기본안

- 기존 FIFO 및 용량 초과 시 drop-oldest 정책을 유지합니다.
- 기존 `FrameBuffer`와 `BackendPipeline`을 재사용하며, 새로운 queue,
  worker thread 또는 동기화 기능은 도입하지 않습니다.
- 현재 single-thread enqueue -> immediate drain 구조와 capacity `1`을 유지합니다.
  최종 운영 capacity는 A-B-C 연동 후 실제 처리율과 지연 측정 결과로 결정합니다.

### 현재 구조의 한계

- consumer가 느리면 같은 thread의 parser 처리와 다음 `recv()`도 늦어집니다.
  현재 즉시 drain 구조에서는 FrameBuffer에 여러 frame이 쌓이는 대신
  OS TCP 수신 buffer에 데이터가 쌓일 수 있습니다.
- FrameBuffer의 drop-oldest는 이미 parsing된 frame에만 적용되므로 TCP backlog를
  해결하지 못합니다. capacity를 늘리는 것만으로 지속적인 처리율 부족을 해결할 수 없습니다.
- 현재 Parser Benchmark는 parser core 처리시간만 측정합니다.
  FrameBuffer 대기, consumer 처리 및 TCP 대기를 포함한 전체 pipeline 지연 측정이 아닙니다.

### 추후 통합 시 확인 및 팀 협의 사항

- 9주차 A-B-C 연동 후 실제 frame 처리시간과 지연을 측정합니다.
- frame 누락 시 C Tracking의 대응 정책을 협의합니다. drop-oldest는 남은 frame의
  순서를 보존하지만 모든 frame의 연속 처리를 보장하지 않습니다.
- 실시간성 요구에 맞는 허용 지연시간과 queue capacity를 결정합니다.
- 필요 시 overflow drop 통계, queue 대기 및 처리시간 계측을 추가합니다.
  현재 queue 길이는 `pendingSize()`로 조회할 수 있지만 drop 통계와 대기시간 계측은 없습니다.
- consumer 오류 시 종료, 계속 처리 또는 재시도 여부를 협의합니다.
- 실제 Radar Emulator 기반 과부하 검증으로 TCP backlog와 처리 지연을 확인합니다.

## 요구 환경

- Windows
- Visual Studio 2022 / MSVC
- CMake
- C++20

### WebSocket 의존성

`cmake/WebSocketDependencies.cmake`는 Boost **1.86.0**과 nlohmann/json **3.11.3**의
공식 배포 archive를 SHA256으로 고정합니다. Boost.Beast/Asio/System은 header-only로
사용하며 Boost 바이너리 빌드나 시스템 전역 설치가 필요하지 않습니다. JSON은
`nlohmann_json::nlohmann_json` CMake target을 사용합니다.

새 configure의 첫 실행은 인터넷과 Boost archive 약 149MB 다운로드·압축 해제가
필요합니다. 소스는 선택한 build directory의 `_deps/`에만 저장됩니다.
버전 고정은 최신 보안 상태나 production 사용 적합성을 보장하지 않습니다.
배포 시 [Boost License](https://www.boost.org/LICENSE_1_0.txt)와
[JSON MIT License](https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT)를 확인하세요.

다운로드가 제한된 환경은 CMake의 `FETCHCONTENT_SOURCE_DIR_ELEVSAFE_BOOST`와
`FETCHCONTENT_SOURCE_DIR_ELEVSAFE_JSON`에 미리 확보한 동일 버전 source directory를
지정할 수 있습니다. 이 override에서는 archive hash 검사가 실행되지 않으므로
관리자가 source의 버전·무결성을 별도로 확인해야 합니다.

### Windows 한글 로그

WebSocket의 Boost.System Windows 오류 메시지는 `BOOST_SYSTEM_USE_UTF8`로
UTF-8을 사용하며, 이 설정은 Boost를 사용하는 서버와 테스트에 함께 적용합니다.
소켓 동작, JSON 및 B의 코드·파일 형식은 변경하지 않습니다.
반복 스크립트도 새 stdout/stderr 로그를 UTF-8로 읽습니다.

B의 한글 로그는 기존 `/utf-8` 빌드의 UTF-8 출력입니다. 기존 UTF-8 파일을 조회할 때는
`Get-Content -LiteralPath .\existing.log -Encoding UTF8`처럼 인코딩을 지정하세요.
직접 실행하는 터미널과 native 출력 캡처 환경도 UTF-8에 맞아야 하며, 프로그램에서
Windows 콘솔 코드 페이지를 강제로 변경하지 않습니다. 이미 잘못 변환되어 저장된
로그나 이전 ACP 오류 메시지가 섞인 파일을 이 설정이 복구하지는 않습니다.
한글 표시와 B의 Unicode 파일 경로 지원은 별개이며 B의 경로 API는 이번에 바꾸지 않습니다.

## Build

프로젝트 루트에서 실행합니다.

```powershell
cmake -S backend -B backend/build-websocket-integration -G "Visual Studio 17 2022" -A x64
cmake --build backend/build-websocket-integration --config Release
```

Repository 루트의 CMake 진입점으로도 같은 Backend와 B `radar_core`를 빌드할 수 있습니다.
기존 독립 빌드 디렉터리는 그대로 유지합니다.

```powershell
cmake -S . -B build-integrated -G "Visual Studio 17 2022" -A x64
cmake --build build-integrated --config Release
ctest --test-dir build-integrated -C Release --output-on-failure
```

루트 빌드의 실행 파일은 `build-integrated/backend/Release/`에 생성됩니다.
아래의 기존 실행 예제를 루트 빌드로 사용할 때는 exe 경로만 이 위치로 바꾸고,
작업 디렉터리는 Repository 루트로 유지합니다. 의존성 소스 재사용 옵션은
[루트 README](../README.md#windows-루트-빌드-및-테스트)를 참고하세요.

## Test

```powershell
ctest --test-dir backend/build-websocket-integration -C Release --output-on-failure
```

통합 빌드의 실행 파일은 `backend/build-websocket-integration/Release/`에 생성됩니다.
아래 명령은 통합 실행 파일을 사용합니다. 기존 `backend/build/`의 입력 데이터와
이전 검증 기록은 그대로 보존하며 새 통합 빌드와 구분합니다.

Windows CMake에 등록된 테스트는 다음 17개입니다. 비 Windows에서는 PowerShell 재연결
fixture를 제외한 16개입니다.

- `retina_protocol_test`: packet/frame parsing 및 stream edge case
- `retina_reconnect_test`: 재시도 횟수·복구 예산, 10014 제외, 사전 취소·만료 기한,
  실제 loopback 연결 후 무응답 수신 대기의 owner-thread 취소
- `radar_reconnect_cli_test` (Windows): 제어된 포트의 연결 거부·재시작·FIN/RST·부분 패킷,
  새 세션의 timestamp/B/계측 초기화, Radar 복구 중 WebSocket 유지
- `frame_buffer_test`: bounded FIFO, FIFO 순서, oldest frame 제거 및 capacity 검증
- `backend_pipeline_test`: frame enqueue, FIFO drain, consumer 동작 및 overflow 전달 검증
- `relative_frame_clock_test`: connection-relative timestamp 계산 및 reset 검증
- `signal_adapter_test`: 데이터/순서/기본값 보존, 빈 frame, 정수 경계 및 범위 초과
- `signal_integration_test`: 설정/초기화, 기존 enqueue/drain에서 B 호출, 빈 frame,
  오류 후 다음 frame 처리, B sample CSV 직접 입력과 Adapter 경로 결과 비교
- `signal_replay_compare_self_test`: 검증용 역변환, 정수 범위/비정상 입력 거부,
  누락·중복·순서 및 상세 필드 불일치 검출, 빈 frame의 enqueue/drain 검증
- `signal_tcp_compare_self_test`: 무작위 꼬리 구간 제외, 논리 프레임 동기화,
  누락·중복·순서·point bit 변경·지문 모호성·EOF·deadline 및 완료 후 동결 검증
- `signal_background_test`: 기존 OFF CLI, ON 직접 기준 생성/Adapter 비교, ON/OFF의
  Parser·동기화 자가 테스트, 실제 활성화 상태·통계 JSON, 누락 맵 및 모드 불일치 거부

- `websocket_message_test`: Mock Snapshot 및 수동 요청 JSON 검증
- `ab_profile_test`: 빈 통계·분위수·warm-up·저장 상한·50ms 경계·ID 및 교체 카운터 검증
- `ab_profile_replay_test`: 기존 오프라인 동등성 경로와 성능 Helper 연결 검증
- `websocket_server_test`: loopback handshake, 경로, 다중 접속, 요청 FIFO,
  느린 client, 큐 교체, 큰 메시지, 연결 해제/재접속, pending I/O 종료
- `websocket_mock_scenarios_test`: 시험용 가변 Snapshot 6개를 두 client에 전달,
  잘못된 시간/JSON publish 거부 및 송신 client 종료 후 접수 명령 FIFO 보존
- `websocket_cli_test`: 기존 CLI 오류, Mock 실패 시 Radar 미진입, WebSocket 시작 실패 후
  Radar 계속 실행을 검증합니다. Windows 기본 PowerShell/.NET fixture가 임시 loopback 포트로
  두 프레임을 보내 Radar-only 및 WebSocket 실패 모드의 B 처리·정상 EOF를 확인합니다.
  기존 Parser 테스트의 패킷 형식을 사용하며 외부 29172 서버나 새 런타임 설치에 의존하지 않습니다.
  fixture 소켓 오류는 실제 코드와 함께 실패 처리하며 재시도하지 않습니다. 개별 자식 프로세스는
  제한 시간 내에 끝나야 하며 fixture 실패 로그는 실행 파일 옆 `cli-radar-fixture/`에 보존합니다.

테스트는 로컬의 실제 배경 맵에 의존하지 않습니다. Background 테스트만 빌드 폴더의
새 임시 디렉터리에 작은 맵과 설정 사본을 만들며 실제 센서 설치 설정/맵은 변경하지 않습니다.

자체 서버 테스트는 port=0으로 OS가 할당한 포트를 사용하고 timeout 및 직렬 실행을 유지합니다.
외부 제품 Mock에 접속하는 `mock-process`와 `door-sim-process` 시험은 8080을 사용합니다.
외부 Mock 시험은 8080의 서버가 시험 대상인지 먼저 확인하세요. 로컬 반복 검증에서는 listen/connect
단계의 Winsock 10014가 간헐 관찰되었습니다. 전체 테스트 통과와 별개로 반복 안정성은
추가 확인이 필요합니다. 기존 A-B `backend/build` 실행 파일과 혼동하지 마세요.

서버 테스트 실패 시 하위 테스트명, 작업 단계, port, 오류 코드·메시지와 서버 내부 로그를
출력합니다. 포트 충돌 테스트는 주소 사용 중 오류만 인정하며 10014 등 다른 오류는 실패입니다.

상태 대기는 `slow.client.ready`, `snapshot.pressure.observed`, `fast.client.ready`,
`slow.client.retained-or-removed` 등으로 구분하며 기대값·관측값·통계와 Unix epoch ms를
기록합니다. 느린 client는 유지되거나 기존 정책에 따라 제거될 수 있습니다. 테스트는
두 경우 모두 큐 상한, Snapshot 교체/폐기 통계, 정상 client의 실제 송수신을 확인합니다.

하위 테스트를 분리하거나 진행·정리 로그를 실시간으로 보려면 다음처럼 실행합니다.

```powershell
.\backend\build-websocket-integration\Release\websocket_server_test.exe --diagnostics --case queue-bounds-slow-client
& .\backend\tests\repeat_websocket_tests.ps1 -Mode Server -Count 20
& .\backend\tests\repeat_websocket_tests.ps1 -Mode Server -Case queue-bounds-slow-client -Count 20
& .\backend\tests\repeat_websocket_tests.ps1 -Mode Mock -Count 20
```

반복 스크립트는 Windows PowerShell 5.1용이며 기본 프로세스 제한은 45초입니다.
기본 실행 파일 위치는 통합 빌드이며 다른 빌드는 `-BuildDirectory`로 지정합니다.
`backend/build-websocket-integration/diagnostics/<실행시각>-<모드>/`에 전체 stdout/stderr, 시작 즉시
PID·시각을 기록한 `runs.jsonl`과 `results.csv`를 남깁니다. 시작/종료/Timeout 진단은 별도 행이므로
집계할 때 Round별 마지막 행만 사용합니다. `Success`, `Winsock10014`, `Winsock10048`,
`Timeout`, `Other`, `OutputIncomplete`를 구분하며 기존 로그 디렉터리는 덮어쓰지 않습니다.
Timeout 사실을 먼저 저장하고 프로세스·스레드·TCP 상태를 추가 저장한 뒤 중단합니다. 프로세스를 자동 종료하거나
다음 반복을 시작하지 않으므로, 기록된 PID를 확인하고 정리한 뒤 별도 실행하세요.
Timeout 이후에도 로그 redirection을 유지하려면 실행한 PowerShell 창을 프로세스 종료 전까지
닫지 마세요. 실패 지점(`FailureStage`)과 정리 중 마지막 진행 단계(`LastStage`)는 별도입니다.
`-OutputDirectory`로 비어 있는 출력 디렉터리를 지정할 수 있습니다. Mock의 기본 실행 시간은
1초이며 `-MockDurationSeconds`로 변경할 수 있습니다. 출력 파일은 실행 중에도 조회할 수 있습니다.

실행 정책이 `.ps1` 실행을 차단하면 정책을 임의로 변경하지 마세요. 스크립트 실행 방식은
해당 환경의 보안 정책에 따라 확인해야 합니다. 진단 파일에는 로컬 경로·PID 등이 포함됩니다.

### 오프라인 Adapter 비교

`signal_replay_compare`는 같은 `readFramesCSV()` 입력을 B 직접 경로와
`RadarFrame -> BackendPipeline -> toSignalFrame() -> B` 경로로 처리합니다.
독립적인 B 객체 두 개를 유지하고, capacity 1에서 매 frame 즉시 drain하여 이력과 순서를 보존합니다.
설정, 기존 Adapter 및 B 알고리즘은 변경하지 않습니다. TCP/Emulator 검증은 아닙니다.

```powershell
$base = '.\backend\build\s0-3-b-baseline-20261031_014816' # 확보한 검증 자료 폴더
$inputCsv = "$base\S0-3_t01.csv"
$baselineCsv = "$base\b_frames.csv"
$outputDir = '.\backend\build\s0-3-compare' # 새 디렉터리 또는 빈 디렉터리
.\backend\build-websocket-integration\Release\signal_replay_compare.exe `
  signal_processing/config/elevator_1st.cfg `
  $inputCsv $baselineCsv $outputDir --s0-3
```

입력은 PCR 변환기의 raw 8컬럼 `frame,timestamp_us,x,y,z,doppler,power,target_id` 형식입니다.
정수 µs와 유한한 float32 값만 허용하며, 반복 frame 구간·순서 역행·frame 내부 시간 불일치는
B Reader의 정렬/병합 전에 거부합니다. 빈 frame은 메모리 경로에서 허용하지만 point 단위 CSV로
복원할 수 없으므로 가짜 point를 추가하지 않습니다. B 처리 후 CSV는 입력으로 사용하지 않습니다.

옵션 생략 시 두 경로 모두 실제 배경 제거 비활성 상태를 요구합니다. `--require-background`는
두 경로 모두 `usesBackground()==true`를 요구하며, 로드 실패로 OFF fallback되면 실패합니다.
설정/맵을 켜거나 끄는 옵션은 아닙니다. 현재 `elevator_1st.cfg`의 맵이 로컬에 있으면 위 OFF
예시는 거부될 수 있습니다. OFF 재검증은 `background.map_path`가 빈 검증용 설정 사본으로
진행하고, 원본 설정이나 맵을 수정·이동·삭제하지 마세요.

ON 모드는 OFF 기준 `b_frames.csv`를 받지 않습니다. 아래처럼 외부 baseline 인자를 생략하면
실제 ON인 B 직접 처리로 새 `direct_frames.csv`를 생성하고 Adapter 경로와 비교합니다.

```powershell
$base = '.\backend\build\s0-3-b-baseline-20261031_014816'
$out = ".\backend\build-websocket-integration\validation\s0-3-bg-on-offline-$(Get-Date -Format 'yyyyMMdd_HHmmss')"
.\backend\build-websocket-integration\Release\signal_replay_compare.exe `
  signal_processing/config/elevator_1st.cfg "$base\S0-3_t01.csv" $out --require-background --s0-3
$LASTEXITCODE
Get-Content "$out\comparison_summary.json"
```

`--s0-3`은 close threshold 0.5와
1,673 frame / 381,730 point / 333 cluster / 전환 7 / OPEN 1,175 / CLOSED 489 / UNKNOWN 9를
추가 검증합니다. 옵션을 생략하면 다른 raw CSV와 해당 기준 결과에도 사용할 수 있습니다.
이 수치는 로컬 B 단독 재현 기준이며 B 팀원의 프레임별 원본 결과와 직접 대조한 것은 아닙니다.

새 디렉터리 또는 빈 디렉터리만 출력 대상으로 허용합니다. `backend/build/` 아래 새 경로를
사용하면 Git에서 제외되며 기존 기준 파일을 덮어쓰지 않습니다. 생성 파일은
`direct_frames.csv`, `adapter_frames.csv`, `frame_diff.csv`, `comparison_summary.json`입니다.
프레임 CSV는 `frame_id,num_clusters,door_state`이며 timestamp 보존도 내부에서 비교합니다.
입력 값과 cluster 상세 float는 동일 코드의 직접 복사를 검증하므로 허용 오차 없이 float32 비트로
비교합니다. 누락·중복·순서·기준 결과 차이를 검출하면 최초 차이를 출력하고 종료 코드 1을 반환합니다.
입력/초기화 오류는 stderr와 종료 코드 1로 보고하며 완료 결과 JSON을 생성하지 않을 수 있습니다.

### WebSocket 단독 Mock

Radar Emulator 없이 프로젝트 루트에서 실행할 수 있습니다.

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe --ws-mock
.\backend\build-websocket-integration\Release\elevsafe_backend.exe --ws-mock --ws-port 8080 --ws-duration 60
```

기본 주소는 `ws://127.0.0.1:8080/radar`, Mock 실행 시간은 30초입니다. 고정된 예시
Snapshot을 목표 100ms(10Hz) 주기로 publish하며 `Ctrl+C` 또는 지정 시간이 끝나면 서버를 정리합니다.
연결 직전 publish된 Snapshot은 재전송하지 않으므로 첫 수신은 다음 주기까지 기다릴 수 있습니다.
Mock의 timestamp는 `2026-10-30T11:00:00Z`라는 **고정 예시**이며 현재 시각이나 센서 시각이 아닙니다.
승객 label도 `MOCK Passenger #1`로 구분합니다. `timestamp_us`는 Mock 세션 첫 발행을
0으로 하는 steady clock 경과 µs(정수)이며 재접속으로 초기화하지 않습니다. 실제 RadarFrame의
시각이나 처리 지연이 아닙니다. 스케줄이 늦으면 놓친 주기를 건너뛰며 몰아서 전송하지 않습니다.
10Hz는 목표 전송 주기이며 OS 스케줄링·큐 교체·느린 클라이언트에 따른 지연/누락이 가능합니다.
Radar 시연 모드는 모든 B 프레임을 C에 전달하고 발행만 최대 약 10Hz로 제한합니다. 입력 중단 시 재전송하지 않습니다.

`--mock-websocket`은 기존 `--ws-mock`의 별칭입니다. Mock은 host/port,
benchmark 및 `--signal-config`와 함께 사용할 수 없습니다.

### 레이더 수신과 WebSocket transport 동시 실행

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172 --websocket
```

이 모드는 기존 TCP/Parser/FrameBuffer/console 경로와 WebSocket 접속·요청 수신을
함께 실행합니다. 위 명령에는 `--signal-config`가 없으므로 실시간 Snapshot은 생성하지 않습니다. B 설정을 함께 지정하면 아래 시연용 Snapshot을 발행합니다.
옵션을 생략하면 서버를 시작하지 않고 기존 동작을 유지합니다. Radar 모드에서 WebSocket
시작이 실패하면 원래 예외 정보를 `[ws] startup failed:`로 출력하고 실패 객체를 정리합니다.
`[ws] disabled for this run; continuing radar processing`을 기록한 뒤 활성 WebSocket 없이
Radar TCP 수신과 선택적 B 처리를 계속합니다. 재시도는 하지 않으며 최종 종료 코드는
기본 모드는 기존 Radar 실행 결과를 따릅니다. TCP 실패/정상 EOF로 run이 반환하면 서버도
정리합니다. 아래 선택적 재연결 모드에서는 복구 대기 중 같은 WebSocket 객체를 유지하고,
최종 종료할 때 정리합니다.
Mock 전용 모드의 시작/실행 실패는 기존처럼 종료 코드 1이며 Radar 연결을 시도하지 않습니다.
CLI 오류와 B 초기화 실패 정책은 유지합니다. 메모리 할당 실패와 표준 라이브러리의
thread/mutex 오류는 치명적 오류로 종료하며 Radar 계속 실행 대상으로 취급하지 않습니다.
이 정책은 시작 실패의 전파 범위만 제한합니다. 기존 서버 정리의 무제한 대기 가능성이나
Winsock 10014의 근본 원인을 해결한 것이 아닙니다.
레이더 모드의 기존 Ctrl+C 종료 처리는 변경하지 않았으므로 그 경로의 graceful shutdown은
보장하지 않습니다. WebSocket thread에서 `RetinaClient::stop()`을 호출하지 않습니다.

| 옵션 | 의미 |
| --- | --- |
| `--websocket` | 레이더와 함께 transport 활성화 |
| `--ws-mock` | 독립 Mock, 레이더 연결 없음 |
| `--ws-bind IP` | 기본 `127.0.0.1`; IP literal 사용 |
| `--ws-port N` | 기본 8080, 1~65535 |
| `--ws-duration N` | Mock만 사용, 1~86400초 |

주소/port 옵션은 `--websocket` 또는 `--ws-mock`이 필요합니다. Mock은 레이더 host/port 및
Benchmark와 함께 사용할 수 없습니다. 설정 오류는 종료 코드 2입니다.
기존 `--benchmark`/`--benchmark-frames`의 Parser 측정 범위는 변경하지 않았습니다.

### 같은 PC 및 LAN PC에서 접속

기본 bind는 `127.0.0.1:8080`이며 경로는 `/radar`입니다. 기본값은 같은 PC의
클라이언트용으로 유지합니다. 같은 LAN의 다른 PC에서 접속하려면 Backend PC에 실제로
할당된 LAN IPv4를 `--ws-bind`로 지정합니다. 이 옵션에는 호스트명이 아닌 IP를 사용합니다.

```powershell
# Backend PC: 먼저 활성 네트워크 어댑터의 IPv4를 확인합니다.
Get-NetIPConfiguration
$backendLanIp = '<Backend PC의 실제 LAN IPv4>' # 확인한 주소로 교체
.\build-integrated\backend\Release\elevsafe_backend.exe --ws-mock `
  --ws-bind $backendLanIp --ws-port 8080 --ws-duration 60
```

Backend 독립 빌드는 실행 파일 경로를 `backend/build-websocket-integration/Release/`로
바꾸면 됩니다. `--ws-port`를 바꾸면 클라이언트 URL의 포트도 함께 바꿉니다.

| Backend bind | 같은 PC의 URL | 다른 LAN PC의 URL |
| --- | --- | --- |
| `127.0.0.1` (기본) | `ws://127.0.0.1:8080/radar` | 접속 불가 |
| Backend의 LAN IPv4 | `ws://<Backend LAN IPv4>:8080/radar` | 같은 URL 사용 |
| `0.0.0.0` (명시적으로 선택할 때만) | loopback 또는 Backend LAN IPv4 URL | Backend LAN IPv4 URL |

`localhost`와 `127.0.0.1`은 브라우저/클라이언트가 실행되는 PC 자신을 가리킵니다.
Frontend를 여는 PC가 Backend와 다르면 Backend의 LAN IP를 사용해야 합니다.
`0.0.0.0`은 모든 IPv4 인터페이스의 수신 주소이며 클라이언트 접속 URL로 사용하지 않습니다.
실제 LAN IP만 bind하면 loopback URL에서는 수신하지 않습니다. DHCP나 네트워크 변경으로
LAN IP가 바뀌면 실행 인자와 클라이언트 URL을 다시 맞춰야 합니다.

동일 LAN이어도 Windows 방화벽, 활성 네트워크 프로필, 단말 격리 등에 따라 원격 연결이
차단될 수 있습니다. 필요한 인바운드 허용은 별도 승인 후 Backend 실행 파일·TCP 포트·
허용할 D PC IP·실제 적용 프로필로 제한합니다. Private 전용 규칙은 Public 프로필에
적용되지 않습니다. 서버는 방화벽/프로필을 자동 변경하지 않습니다.
같은 PC에서 LAN IP로 접속한 성공은 다른 PC에서의 접속 성공을 뜻하지 않습니다.

2026-11-01에는 기존 Release 실행 파일을 활성 LAN IPv4의 8080에 bind해 동일 PC에서
검증했습니다. 두 클라이언트의 handshake와 동일 v6 JSON 21개 수신, 증가하는 정수
`timestamp_us`, 평균 Mock 시간 간격 99.68ms(수신 간격 90.78ms)를 확인했습니다.
초기 수신 큐와 스케줄링이 있으므로 이 짧은 표본은 정확한 실시간 주기 보장이 아닙니다.
한 연결 종료 후 다른 연결의 수신 유지, OPEN/HOLD/CLOSE 순서의 요청 수신,
잘못된 요청 거부 및 서버/클라이언트 종료 코드 0을 확인했습니다. 실제 문 제어는 없습니다.
이 실행에서는 10014가 없었으며 반복 안정성이나 다른 PC의 방화벽/라우팅/D GUI 접속은
미검증입니다. 원시 로그와 결과는 Git 제외 경로
`build-integrated/diagnostics/ws-lan-20261101-01/`에 보존했습니다.

### JSON 및 수동 요청

v7 송신은 `timestamp`, `timestamp_us`, `door_command`, `door_state`, `passengers`를
포함한 text JSON입니다. `timestamp_us`는 세션 첫 프레임 기준 경과 µs의 음수가 아닌 정수입니다.
누락, 음수, 실수, 문자열, null/bool은 거부하며 별도 `alert` 필드는 허용하지 않습니다.
`timestamp`는 유지하며 상대 µs를 절대시각이나 처리 지연으로 해석하지 않습니다.
승객은 `id`, 유한한 숫자 `x/y`(m), `state`, `safety_state`, 선택적 `label`을 사용합니다.
`state`는 OUTSIDE/ENTERING/INSIDE/EXITING, `safety_state`는 null/FALL/IMMOBILE입니다.
door_state는 v7의 OPEN/CLOSED/OPENING/CLOSING/UNKNOWN을 허용하며 door_command는 null/OPEN/HOLD/CLOSE입니다.
UNKNOWN은 미확정 상태로 보존하며 CLOSED로 변환하지 않습니다. z 필드는 추가하지 않습니다. ID는 정수 또는 문자열을 허용하며
최종 Tracking ID 타입과 실제 결과 공급은 C 통합 시 확인해야 합니다.

현재 수동 요청은 다음처럼 **door_command 한 필드만** 받습니다.

```json
{"door_command":"OPEN"}
```

OPEN/HOLD/CLOSE는 로그와 `popCommand()` FIFO에 전달합니다. JSON 오류, 비객체,
필드 누락, null/알 수 없는 명령, 추가 필드, binary 메시지는 기본 모드에서 거부합니다.
예외적으로 `--ws-mock --ws-door-sim`에서만 단일 필드 `{"door_command":null}`을
유효한 무동작 알림으로 수용합니다. FIFO와 실행 명령 통계에는 넣지 않습니다.
**수신은 실행 완료가 아닙니다.** ACK는 없습니다. 기본 Mock에서는 수신 큐를 비우지만
요청에 따라 Mock 상태를 바꾸지 않습니다. 아래의 선택적 문 시뮬레이션에서만
소프트웨어 문 상태와 MANUAL/AUTO 전환을 적용합니다.
레이더 모드의 큐는 향후 제어 계층용이며 현재 실행 consumer는 연결하지 않았습니다.

### 스레드·큐·종료 정책

- 서버 객체가 I/O thread 하나와 모든 세션을 소유합니다. start/stop은 소유자 thread에서 호출합니다.
- `publishSnapshot()`은 소유권 있는 JSON을 전달하며 네트워크 전송 완료를 기다리지 않습니다.
- ingress 최신 메시지 1개와 알림 작업 1개만 대기합니다. 세션마다 전송 중 1개를 보존하고
  대기 1개만 최신 메시지로 교체합니다. 동일 세션의 write는 직렬 실행합니다.
- 기본 최대 client 8개(진행 중 handshake 포함), Snapshot 65536 bytes, 수동 요청 4096 bytes,
  명령 큐 32개입니다. HTTP header는 8192 bytes, body는 허용하지 않습니다.
- listen backlog는 `min(maxClients, 128)`입니다. 로컬 검증에서 시스템 최대값
  SOMAXCONN 사용 시 .NET에서도 Winsock 10014가 재현되어 유한한 backlog를 사용합니다.
  이 접속 대기열은 Snapshot/명령 큐와 별도이며 OS 설정은 변경하지 않습니다.
- 명령 큐 초과 시 기존 요청을 보존하고 새 요청을 수용하지 않은 채 해당 client를 종료합니다.
  Snapshot처럼 명령을 조용히 덮어쓰지 않습니다. 큰 요청은 연결을 종료합니다.
- handshake 읽기 제한 3초, write 제한 5초입니다. 느린 client의 세션만 닫으며 Radar TCP 설정은 바꾸지 않습니다.
- ingress 큐는 1개, 등록 세션마다 전송 중 1개·대기 1개를 보관합니다. 기본 8개 세션의
  정상 진행 중 payload는 전달 중 메시지를 포함해 보수적으로 약 18 x 64KiB입니다.
  취소 완료를 기다리는 종료 세션의 전송 buffer, 수신 buffer, JSON 임시 메모리와 OS buffer는
  별도이므로 전체 프로세스 메모리에 대한 고정 상한을 뜻하지는 않습니다.
- stop은 accept 중단 후 close handshake를 시도하고 500ms 후 남은 socket을 취소/닫습니다.
  thread를 join하고 남은 completion handler도 정리합니다. stop은 반복 호출할 수 있고,
  재시작은 새 서버 객체를 생성합니다. 500ms는 스케줄링 지연까지 포함한 실행시간 보장이 아닙니다.
- I/O handler 예외 시 새 연결을 중단하고 남은 작업을 취소·정리합니다. start 실패와 이미
  종료된 I/O thread도 stop의 정리 대상입니다. start 실패 후 재시도 역시 새 객체가 필요합니다.
- stop은 I/O 종료 요청을 보낸 뒤 thread 종료 통지를 기다리고 join합니다. 지연 시
  `stop.thread.wait`, 취소 완료 처리 지연 시 `stop.drain.wait`를 출력합니다. 테스트 client도
  취소된 callback 완료까지 객체·버퍼를 보존하며 `client.cancel.drain` 진행 상태를 출력합니다.
  native 취소나 handler가 멈추면 안전한 자원 해제를 위해 대기가 남을 수 있습니다.
  hard timeout 보장, 강제 thread 종료, detach는 제공하지 않으며 외부 watchdog으로 진단합니다.
- `statistics()`에서 ingress/session 교체 수, 송신 수, 수신·거부 및 명령 큐 초과를 조회합니다.
  연결 종료·write timeout·Snapshot 폐기, 현재/최대 ingress·세션 대기·write 수도 조회합니다.
  각 카운터는 원자적이지만 여러 필드가 한 순간의 일관된 snapshot을 보장하지는 않습니다.
  Mock 종료 로그도 교체/요청 통계를 출력합니다. 서버 자체는 영구 로그 파일을 만들지 않습니다.

### D Frontend 수동 연결 절차

D 코드 변경은 이번 작업에 포함하지 않습니다. 이 Backend 브랜치의 로컬
`frontend/src/App.jsx`는 연결·명령 송신 코드가 주석 처리된 이전 상태입니다.
별도로 확인한 GitHub main `bfa1d5af`의 Frontend에는 실제 연결·송신 코드가 있으며,
소스 상수 `USE_REAL_WEBSOCKET=false`, `RADAR_WS_URL='ws://localhost:8080/radar'`를
사용합니다. 환경변수 설정이 아니므로 `npm start`만으로 실제 연결이 활성화되지는 않습니다.
이 설명은 과거 main 기준입니다. 별도로 확보한 D팀 `feature/ui-dashboard`
`f5fcb511785cb8c1c67830f7b8d8e57f58241ebd`는 `USE_REAL_WEBSOCKET=true`,
`RADAR_WS_URL='ws://127.0.0.1:8080/radar'`로 연결이 활성화돼 있습니다.
사용하는 D팀 버전을 구분해야 하며 Backend 자체 시험과 실제 화면 검증은 별개입니다.
사용자 수동 검증 결과(PR #27 기록): 실제 A–D 화면의 OPEN/CLOSE/HOLD 및 null 처리 정상 확인.
이는 소프트웨어 Mock 검증이며 실제 C 판단·물리 문 제어·완료 ACK 검증이 아닙니다.

초기 A–D 시험은 **동일 PC의 loopback**으로 제한합니다. Backend 실행 후 `[ws] listening`
로그를 확인하고 D는 `ws://127.0.0.1:8080/radar`에 연결합니다. `localhost`는 브라우저가
실행되는 PC 자신을 뜻합니다. 다른 PC 접속은 별도 승인된 LAN 시험 대상으로 남깁니다.

```powershell
# Repository 루트에서 실행. 아래 경로는 기본 루트 Release Build 기준입니다.
Set-Location 'C:\Users\kimji\ElevSafe'
& '.\build-integrated\backend\Release\elevsafe_backend.exe' --ws-mock --ws-bind 127.0.0.1 --ws-port 8080 --ws-duration 60
```

격리 Build에서는 위 EXE를 해당 Build의 `backend/Release/elevsafe_backend.exe`로 교체합니다.
서버는 지정 시간이 끝나거나 Ctrl+C를 받으면 종료합니다. 클라이언트 하나가 종료돼도 다른
연결과 발행은 유지하며, 재접속한 클라이언트는 다음 발행부터 받습니다. 서버는 클라이언트의
자동 재연결을 대신하지 않으며, 연결 중단 후 오래된 표시를 어떻게 처리할지는 D에서 확인해야 합니다.

Backend → Frontend text JSON v6 예시(합성 데이터이며 실제 C 결과가 아님):

```json
{"timestamp":"2026-10-30T11:00:00Z","timestamp_us":100000,"door_state":"OPEN","door_command":null,"passengers":[{"id":1,"x":0.25,"y":0.8,"state":"INSIDE","safety_state":null,"label":"MOCK Passenger #1"}]}
```

`passengers`는 현재 표시할 **전체 목록**으로 취급하고 이전 목록에 누적하지 않습니다.
`passengers: []`이면 기존 승객 표시를 비웁니다. x/y는 m이며 Timestamp는 지연 측정값이
아닙니다. `timestamp` 검사는 문자열 타입만 확인하고 ISO 날짜의 유효성까지 검사하지 않습니다.
`timestamp_us` 타입은 검증하지만 서버가 메시지 간 단조 증가나 세션 초기화를 강제하지는 않습니다.
이번 시험용 시퀀스와 기본 Mock 발행자가 각자의 상대 시간을 생성합니다.

Frontend → Backend 요청은 각각 별도 text 메시지로 보냅니다.

```json
{"door_command":"OPEN"}
{"door_command":"HOLD"}
{"door_command":"CLOSE"}
```

연결이 OPEN일 때 보내고 Backend의 `manual request received ... (not executed; no ACK)`
로그로 접수를 확인합니다. 이는 실제 문 실행·완료·ACK가 아닙니다. FIFO 순서는 Backend가
접수한 순서이며 서로 다른 클라이언트의 송신 시각 순서를 보장하지 않습니다. 접수된 명령은
송신 클라이언트가 종료돼도 큐에 남고, 만료 시각이나 종료 시 취소 정책은 없습니다.
기본 Mock은 FIFO를 소비해 폐기하고 문 상태를 바꾸지 않습니다. 모든 Mock Snapshot의
`door_command=null`은 개별 수동 요청의 ACK나 거부 응답으로 해석하지 마세요.
기본 Mock은 실제 C Tracking을 사용하지 않습니다. Radar 시연 연결은 아래 별도 절을 참고하세요. 물리 문 상태 확인은 미연동입니다.

인증·Origin 검증·접속 IP별 권한·TLS는 구현하지 않았습니다. LAN에서 접근 가능한
클라이언트는 Snapshot 수신뿐 아니라 OPEN/HOLD/CLOSE 요청도 보낼 수 있습니다.
현재는 수신·FIFO까지만 구현되어 실제 문 제어/ACK는 없지만, 임의 요청과 큐 포화 위험이
있으므로 신뢰된 LAN의 허용된 시험 PC로 접근을 제한해야 합니다. 외부 인터넷 공개나
인터넷 포트 포워딩을 하지 마세요. HTTPS UI에는 `wss://`가 필요하지만 현재 서버는
TLS를 제공하지 않습니다. LAN 시험은 승인된 환경에서 진행하며 실제 문 제어 시스템에는
연결하지 않습니다.

### 선택적 소프트웨어 문 시뮬레이션

```powershell
& '.\build-integrated\backend\Release\elevsafe_backend.exe' --ws-mock --ws-door-sim --ws-bind 127.0.0.1 --ws-port 8080 --ws-duration 60
```

`--ws-door-sim`은 `--ws-mock`(별칭 `--mock-websocket`)에서만 사용할 수 있습니다.
Radar 모드에서 사용하거나 중복 지정하면 ExitCode 2로 종료합니다.
기본 `--ws-mock`은 계속 OPEN/null, 고정 승객 1명, 목표 10Hz이며 접수 명령을 폐기합니다.
새 옵션을 지정한 경우에만 기존 FIFO를 소프트웨어 상태 머신이 소비합니다. 추가 스레드는 없습니다.

- 초기 상태는 AUTO/CLOSED입니다.
- OPEN: OPENING으로 전환 후 2.68초에 OPEN, AUTO 복귀. 완전히 열린 시점부터 4초 후 자동 CLOSE합니다.
- CLOSE: CLOSING으로 전환 후 2.35초에 CLOSED, AUTO 복귀.
- 이미 목표 상태이면 즉시 처리하고 AUTO로 복귀합니다.
- HOLD: 필요하면 2.68초 동안 OPENING 후 OPEN을 유지하며 MANUAL 상태를 유지합니다.
  후속 OPEN/CLOSE로 해제합니다. OPEN 상태의 HOLD는 이동하지 않습니다.
- 다른 명령은 기존 전환을 취소하고 새 명령의 전체 시간을 적용합니다.
  동일한 활성 명령은 타이머를 재시작하지 않습니다.
- FIFO 접수 순서대로 적용합니다. 한 발행 간격에 여러 명령을 소비하면 마지막으로 적용된
  명령만 다음 Snapshot의 `door_command`에 한 번 표시하고, 이후에는 null을 발행합니다.
  이는 모든 명령의 전달 보장 또는 실행 완료 ACK가 아닙니다. 기존 최신 Snapshot 교체 정책도 유지합니다.
- OPEN 유지 시간은 `DoorSimulation::openHoldDuration`(임시 4000ms) 한곳에서 관리합니다.
  이동 타이머와 분리하며 정상 경계는 0ms OPENING → 2680ms OPEN → 6680ms CLOSING → 9030ms CLOSED입니다.
  초기 CLOSED에서 주기적인 OPEN은 발생하지 않습니다.
- 수동 FIFO를 먼저 처리하고 자동 타이머를 평가합니다. 같은 시각의 MANUAL HOLD/CLOSE가 자동 CLOSE보다 우선합니다.
  자동 CLOSE는 mode=AUTO이며 새 `door_command="CLOSE"` 이벤트를 한 번만 발행합니다.
  다음 Snapshot은 null이며, 느린 클라이언트의 이벤트 전달 보장을 추가하지 않습니다.
- 이미 OPEN인 상태의 반복 OPEN은 진행 중인 유지 타이머를 연장하지 않습니다.
  MANUAL HOLD를 OPEN으로 해제하면 그 시점부터 새 4초를 시작합니다.
- 내부 시험용 `applyAuto()`는 수동 `apply()`와 분리되어 있습니다. MANUAL 중 입력은 실행·예약하지 않습니다.
  AUTO OPEN/HOLD는 일반 자동 CLOSE보다 우선하며 명시적 다른 입력 또는 수동 명령까지 유지합니다.
  입력 없음(nullopt)은 해제나 위험 해소가 아닙니다. 같은 AUTO 입력 반복은 이벤트·이동 타이머를 재시작하지 않습니다.
  수동 명령이 AUTO 입력을 대체한 후 이전 AUTO 요청을 재생하지 않습니다.
- UNKNOWN 초기 상태의 모델에서는 AUTO 입력을 적용하지 않으며 수동 입력은 허용합니다.
  제품 Mock은 계속 CLOSED로 시작합니다. 시험용 UNKNOWN 상태가 B 관측값을 연결했다는 뜻은 아닙니다.
- 접수한 명령은 연결 종료로 삭제하지 않으며, HOLD도 연결 종료만으로 해제되지 않습니다.
  인증, 명령 소유권, 만료, 중복 요청 식별, 완료 ACK는 없습니다.

기본 시뮬레이션은 기존 `mockSnapshot()`의 승객과 시간 필드를 재사용하고 `door_state`와 `door_command`만
변경합니다. JSON 최상위 5개 필드·타입은 그대로이며 v7 UNKNOWN 허용 외에 AUTO/MANUAL 필드를 추가하지 않습니다.
`timestamp_us`는 기존 Mock의 첫 발행 프레임 기준 상대 µs입니다. 제어용 `steady_clock`
완료 시각과 Snapshot 수신 시각은 다릅니다. 상태 전환은 Mock 루프의 다음 갱신에서 관측되고
화면에는 목표 100ms 발행 주기 및 송신 지연을 거쳐 나타납니다.
`[door-sim]` 로그의 mode는 내부 AUTO/MANUAL 확인용이며 네트워크 ACK가 아닙니다.

**소프트웨어 시뮬레이션만 수행합니다.** 물리 장치·문 상태 센서·실제 C Tracking의 자동 판단·
B 문 추정 상태와 연결하지 않습니다. JSON이 실제 문 관측이라고 해석되지 않도록
실행 로그와 이 옵션을 확인해야 합니다.
D팀의 OPEN/CLOSE 이후 알림 `{"door_command":null}`은 시뮬레이션 모드에서만
정상 수신 후 무동작 처리합니다. OPENING/CLOSING 취소, 타이머 변경, HOLD 해제,
강제 AUTO 전환은 하지 않습니다. 다른 클라이언트의 지연된 알림과 반복 알림도 동일합니다.
OPEN/CLOSE의 AUTO 복귀는 상태 머신 자체 완료 시간에 의해 수행합니다.
`null door request no-op` 로그를 남기되 접수 명령 수·거부 수·FIFO는 변경하지 않습니다.
기본 Mock과 Radar 모드는 계속 null을 거부합니다. AUTO 문자열, 잘못된 타입, 추가 필드,
binary 요청은 모두 거부합니다. null은 접수 또는 완료 ACK가 아니며 기존 JSON 최상위 필드는 그대로입니다.
Door Mock에는 C 자동 제어 입력을 연결하지 않았습니다. Radar 시연 경로의 y-only 분류는 C State Machine이 아닙니다.
`applyAuto()`는 로컬 중재 시험용 진입점이며 C 공개 인터페이스를 확정한 것이 아닙니다.
실제 C의 위험 해소·입력 소실 표현과 복구 정책은 별도 합의 대상입니다. 아래 선택형 Mock에서는 시험용 정책을 명시적으로 적용합니다.
위 Door Mock 설명과 아래 Radar 시연의 B UNKNOWN/C Track Snapshot 발행 경로는 구분합니다.
D의 UNKNOWN 수신·'확인 중' 표시와 null 송신 로그 정정은 D팀 작업이며 현재 Frontend는 수정하지 않았습니다.

아래 자동 시험 기록과 위 사용자 수동 A–D 화면 검증 결과를 구분합니다.

`door_simulation_test`는 sleep 없이 가상 시각으로 완료 경계·충돌·HOLD·AUTO 복귀를 검증합니다.
기존 테스트 클라이언트의 `--case door-sim-process --diagnostics`는 별도로 시작한 위 서버의
8080에 접속해 두 클라이언트 상태 일치, 전환 시간, 목표 10Hz, 연결 종료·재접속을 검사합니다.
이 명령은 서버를 직접 시작하지 않습니다. 기본 Mock 검증에는 기존 `--case mock-process`를 사용합니다.

2026-11-06 최초 구현 검증 (`backend/build/diagnostics/door-sim-20261106-01`, null 호환 추가 전 기록):

| 검증 | 결과 |
|---|---|
| Windows MSVC x64 Release 루트 / Backend Build | 양쪽 PASS |
| 루트 CTest | 18/18 PASS, ExitCode 0 |
| Backend 독립 CTest | 17/18 PASS, ExitCode 8 |
| 신규 가상 시각 단위 시험 | Windows 양쪽 및 Ubuntu g++ 실행 PASS |
| 신규 시뮬레이션 실제 서버 / 기존 클라이언트 | 양쪽 ExitCode 0, 두 클라이언트 상태 일치 |
| 기본 Mock 실제 서버 / 기존 클라이언트 | 양쪽 ExitCode 0, 동일 Snapshot 21개 |

Backend 실패는 `radar_reconnect_cli_test`의 `partial-fin-restart` 재시작 fixture에서
발생한 **bind system:10014**입니다. 재시도하지 않았고 근본 원인은 미확정입니다.
신규 시뮬레이션, 기존 WebSocket/JSON/Mock 시나리오, A–B 및 Background 시험은 통과했습니다.
시뮬레이션 평균 수신 간격은 100.05ms, 기본 Mock은 100.94ms였습니다.
내부 로그에서 OPEN/CLOSE 완료 후 AUTO, HOLD 완료 후 MANUAL, 후속 OPEN으로 AUTO 복귀,
null 요청 거부를 확인했습니다. Snapshot 관측 전환 간격은 OPEN 약 2696ms, CLOSE 약 2298ms,
HOLD 열림 약 2699ms이며, 이는 정확한 내부 타이머 기간이나 물리 완료 시간을 뜻하지 않습니다.
경계 시각은 가상 시각 단위 시험에서 별도로 검증했습니다.
당시 Ubuntu에는 CMake가 없어 Linux 전체 CI 구성과 실제 D 화면 연동은 미검증이었습니다.
기본 Mock 시작 전 PID 0 포트 항목이 관측돼 일단 시작을 중단했으며, 후속 조회/netstat에서
FIN_WAIT_1/TIME_WAIT과 listener 부재를 확인한 뒤 미실행 기본 시험을 수행했습니다.
두 실제 서버는 정상 종료했으며 시험 종료 후 listener는 없었습니다. 기존 실패 기록은 보존합니다.

2026-11-06 null 호환 추가 후 검증 (`backend/build/diagnostics/door-null-20261106-01`):

- 루트/Backend 독립 MSVC x64 Release Build PASS. 루트 CTest 18/18 PASS(ExitCode 0),
  Backend CTest 16/18 PASS(ExitCode 8). 두 구성 모두 한 번만 실행했습니다.
- Backend의 `websocket_server_test`(null-compatibility)와 `websocket_mock_scenarios_test`는
  각각 `client.connect`에서 system:10014로 실패했습니다. 원시 Native API 원인은 미확정이며
  실패를 재시도로 대체하지 않았습니다. 루트에서는 두 시험 모두 통과했습니다.
- 루트 시험에서 기본 설정의 null 거부, opt-in null 수용, FIFO/명령·거부 통계,
  가상 시각의 OPEN/CLOSE 경계, HOLD, 다른 클라이언트·반복·지연 null 무동작을 확인했습니다.
  기존 DoorSimulation 헤더·구현·단위 시험은 이번 null 변경 전후 동일합니다.
- 실제 시뮬레이션에서 두 클라이언트 동일 Snapshot, OPEN/CLOSE/HOLD 도중 반복 null,
  완료 후 null, HOLD 해제 OPEN을 확인했습니다. 서버/클라이언트 ExitCode 0,
  실제 명령 4건·거부 0건, 평균 수신 간격 약 99.62ms였습니다.
  기본 Mock도 양쪽 ExitCode 0, 실제 명령 3건·거부 2건(INVALID/null), 평균 약 99.98ms입니다.
- 시뮬레이션 클라이언트 시험은 약 10.97초에 끝났지만 서버 총 경과는 약 432.11초였습니다.
  10:18:16 Modern Standby 복귀 이벤트가 기록됐습니다. 따라서 요청한 22초의 연속 실행과
  종료 시간 보장을 검증한 것으로 보지 않습니다. 재실행하지 않았고 원본 로그를 보존했습니다.
- Ubuntu g++의 JSON 요청/메시지 시험 PASS. 당시 Linux 전체 CMake/CTest 및 실제 D 화면 연동은
  미검증이었습니다. 소프트웨어 전용 범위와 후속 사용자 화면 검증은 위 설명을 참고하세요.

### 자동 CLOSE 및 중재 검증 (2026-11-07)

별도 Worktree의 `backend/build/door-auto-20261107`에서 루트 MSVC x64 Release Configure/Build를 수행했습니다.
Boost 1.86.0과 nlohmann/json 3.11.3의 기존 소스를 재사용했습니다.
전체 CTest는 17/18 PASS(47.20초, ExitCode 8)입니다. 실패는 `websocket_server_test`의
`handshake-snapshots-commands / client.connect`, system:10014이며 원인은 미확정입니다.
실패한 실행을 재시도하지 않았습니다. 해당 하위 시험의 UNKNOWN 송수신 및 뒤쪽 slow-client 시험은 도달하지 못했습니다.
가상 시각의 2679/2680/6679/6680/9029/9030ms 경계, AUTO/MANUAL·HOLD·중복·UNKNOWN 시험,
JSON 시험, 기존 Mock 6개 시나리오, A–B·Background·Radar 재연결 시험은 통과했습니다.

별도의 일반 사용자 loopback Mock 시험은 두 클라이언트 수신, 단발 AUTO CLOSE,
5초 이상 HOLD, null no-op, 수동 OPEN/CLOSE 및 재접속의 내부 assertion을 통과했습니다.
Snapshot 기준 OPEN 유지 관측 구간은 약 3901.90ms, 자동 CLOSING은 2397.45ms,
평균 소스 발행 간격은 100.153ms였습니다. 첫 OPEN 관측 뒤 추가 Snapshot을 읽은 시점부터
유지 시간을 측정하므로 4000ms 가상 시각 경계와 구분합니다.
프로세스 종료 코드 집계가 빈 값으로 남아 실행 래퍼는 ExitCode 1입니다.
내부 TEST_END PASS 및 서버 stop.complete 기록과 구분하며 전체 실행 PASS로 확정하지 않습니다.
원본 stdout/stderr를 보존했고, 종료 코드 누락을 이유로 시험을 반복하지 않았습니다.
실제 C 판단, D 화면 UNKNOWN 표시, 물리 문 제어, Linux 및 별도 Backend 독립 Build는 이번 검증에 포함하지 않았습니다.

### 합성 위험 입력으로 문 제어 시험

기본 `--ws-mock`과 `--ws-door-sim` 동작은 유지합니다. 다음 옵션을 추가할 때만
`MockDoorPolicy`가 합성 Passenger를 해석해 기존 `applyAuto()`에 전달합니다.
Tracking·FALL·IMMOBILE 판정 알고리즘이나 C 공개 인터페이스가 아닙니다.

```powershell
.\backend\build\door-risk-20261107\backend\Release\elevsafe_backend.exe `
  --ws-mock --ws-door-sim --ws-door-scenario risk-clear `
  --ws-bind 127.0.0.1 --ws-port 8080 --ws-duration 20
```

`--ws-door-scenario`는 `--ws-mock --ws-door-sim`과 함께 사용합니다.

| 시나리오 | 합성 입력 및 조작 |
|---|---|
| `normal` | 빈 승객 목록. 클라이언트 OPEN으로 2680/6680/9030ms 기본 사이클 시작 |
| `unknown` | UNKNOWN + ENTERING/FALL. 자동 동작 없음. 수동 OPEN/HOLD/CLOSE로 알려진 상태 전환 |
| `inside-fall`, `inside-immobile` | INSIDE 위험만으로 새 AUTO 명령을 내거나 기존 자동 닫힘 타이머를 중단하지 않음 |
| `entering-fall`, `entering-immobile`, `exiting-fall`, `exiting-immobile` | 알려진 상태에서 AUTO OPEN, 열림 완료 후 HOLD 지속 |
| `risk-clear` | ENTERING/FALL, 시작 8초 후 유효한 빈 배열로 명시적 해제 |
| `input-loss` | ENTERING/FALL, 시작 8초 후 공급 중단. 마지막 유효 입력과 HOLD 유지, 경고 로그 |
| `multi` | INSIDE/IMMOBILE + ENTERING/FALL + OUTSIDE/null. 출입구 위험 우선 |
| `manual-conflict` | 지속 ENTERING/FALL에 클라이언트 OPEN/HOLD/CLOSE를 전송해 중재 확인 |

모든 입력은 Mock입니다. INSIDE/IMMOBILE의 알림은 기존 `safety_state`로 D가 표시하며
최상위 Alert 필드를 추가하지 않습니다. OUTSIDE/FALL·IMMOBILE 및 잘못된 입력은 거부하고,
정상 상태로 바꾸지 않습니다. 입력 소실 시 마지막 Passenger도 유지되므로 새 관측으로 해석하면 안 됩니다.
입력 소실 중에는 새 AUTO 명령을 생성하지 않습니다. 수동 동작 완료 후에도 공급이 복구돼
유효한 위험 입력이 확인돼야 다시 판단합니다. 기존 AUTO HOLD는 그대로 유지합니다.
공급 중단·명시적 입력 무효화와 유효한 빈 배열은 다릅니다. 이 시나리오는 C의 추적 실패나
실제 승객 퇴장을 판정하지 않으며 새 네트워크 입력 규격도 추가하지 않습니다.

Mock의 명시적 위험 해제는 AUTO OPEN/HOLD만 해제합니다. OPEN이면 그 시점부터 4초,
OPENING이면 기존 열림 완료 시점부터 4초 후 자동 CLOSE합니다. CLOSING/CLOSED는 재시작하지
않습니다. 해제 자체는 명령 이벤트나 ACK를 만들지 않습니다. MANUAL HOLD는 해제하지 않습니다.
입력 소실만으로 AUTO HOLD를 해제하지 않으며 무기한 유지 정책은 실제 설비에 적용하지 않습니다.

처리 순서는 수동 FIFO → 이동 완료 반영 → Mock 위험 판단 → 기본 자동 닫힘입니다.
UNKNOWN에서 AUTO 명령은 차단합니다. 수동 CLOSE는 위험 AUTO OPEN/HOLD보다 우선하고,
수동 OPEN/CLOSE 완료 후 위험이 유지되면 다시 평가합니다. 수동 HOLD는 OPEN/CLOSE로만
해제합니다. D의 null과 연결 종료는 해제 요청이 아닙니다. 이는 소프트웨어 시연 정책이며
물리적 안전 인터록이 아닙니다. 복수 승객 판단은 배열 순서와 무관합니다.

새 CTest `websocket_door_risk_test`는 가상 시각으로 정책을 진행하면서 실제 WebSocket의
두 클라이언트에 UNKNOWN·위험·해제 결과를 전송합니다. 가상 시각 시험을 실제 10Hz 시험으로
해석하지 않습니다. 실제 프로세스 시험은 위 서버와 별도로 다음 클라이언트를 실행합니다.

```powershell
.\backend\build\door-risk-20261107\backend\Release\websocket_server_test.exe `
  --case door-risk-process --diagnostics
```

이 클라이언트는 `risk-clear` 시나리오의 OPENING→OPEN/HOLD→해제→CLOSING→CLOSED,
null no-op, 동일 Snapshot 수신을 검사합니다. 서버 준비 후 초기에 연결해야 하며,
서버와 클라이언트의 종료 코드도 별도로 확인합니다. 기존 10014 실패 기록은 위에 보존합니다.

Frontend는 변경하지 않았습니다. 현재 D는 UNKNOWN을 수신 상태에 반영하지 않습니다.
D팀은 UNKNOWN 수신 허용, ‘확인 중’ 표시, 이전 문 상태/애니메이션으로 추정 금지,
null을 ACK·AUTO 복귀로 설명하는 로그 정정을 별도 반영해야 합니다.
실제 C 연결 시 합성 입력과 Mock 정책을 교체하고, WebSocket·FIFO·문 시뮬레이션을 재사용합니다.
C의 위험 해제·입력 소실·갱신 주기와 물리 제어 정책은 후속 합의 범위입니다.

2026-11-07 위험 Mock 확장 검증 로그는 Git 제외 경로
`backend/build/door-risk-20261107` 및 `backend/build/door-risk-backend-20261107`에 보존했습니다.

| 검증 | 결과 |
|---|---|
| 수정 전 루트 Release Build / CTest | PASS / 18/18 PASS, 45.24초, ExitCode 0 |
| 위험 정책 1차 루트 Release Build / CTest | PASS / 19/19 PASS, 45.06초, ExitCode 0 |
| 최종 입력 소실 중 신규 명령 차단 후 루트 Build / CTest | PASS / 19/19 PASS, 42.64초, ExitCode 0 |
| Backend 독립 Release Build / 전체 CTest (1차) | PASS / 18/19 PASS, 43.91초, ExitCode 8 |
| 최종 Backend 독립 Build / 정책 단위 시험 | PASS / PASS, ExitCode 0. 전체 CTest 실패는 재실행하지 않음 |
| 변경 없는 Frontend Build | PASS, ExitCode 0. 기존 main의 동일 소스·lockfile 및 설치된 의존성 사용 |
| 위험 정책 단위 시험 / 12개 WebSocket 시나리오 | 양쪽 구성 PASS |
| 1차 실제 `risk-clear` 서버·클라이언트·래퍼 | 모두 ExitCode 0, 두 클라이언트 142개 동일 Snapshot, CLOSE 이벤트 1회 |
| 최종 바이너리 `risk-clear` 서버·클라이언트·래퍼 | 모두 ExitCode 0, 두 클라이언트 143개 동일 Snapshot, CLOSE 이벤트 1회 |
| 기본 Door Mock 실제 서버·클라이언트·래퍼 | 모두 ExitCode 0, 평균 수신 99.95ms, null/HOLD/재접속 PASS |

Backend 실패는 기존 `websocket_server_test`의 `null-compatibility / client.connect`,
port 58563, system:10014입니다. listen은 성공했으며 원인은 미확정입니다. 이 실행을
재시도하지 않았고 전체 Windows 검증을 모두 PASS로 해석하지 않습니다. 해당 실행의
뒤쪽 서버 하위 시험은 미도달했지만 루트에서는 통과했습니다. 신규 위험 시험은 독립된
CTest로 실행되어 양쪽 모두 통과했습니다. A–B·Background ON/OFF·Radar 재연결도 통과했습니다.

가상 시각의 내부 경계는 2680/6680/9030ms입니다. 기본 Mock에서 관측한 OPEN 유지
3886.34ms는 첫 OPEN 이후 추가 Snapshot을 읽는 기존 클라이언트 측 측정 기준을 포함하므로
내부 4000ms 유지 시간과 구분합니다. 실제 측정 원자료나 물리 문 동작을 검증한 것은 아닙니다.
`risk-clear` 종료 직후 netstat에 일시적인 PID 0 LISTENING 항목이 있었으며 원인은 미확정입니다.
후속 실행 전에는 없었고 최종 조회에서 시험 프로세스와 8080 listener는 남아 있지 않았습니다.
과거 10014 및 Mock 래퍼 종료 코드 누락 기록은 삭제하거나 성공 기록으로 대체하지 않았습니다.
최종 보완은 입력 소실 중 새로운 AUTO 명령을 차단하고 유효 입력 복구 후 재평가하도록 한 것입니다.
기존 HOLD 유지·명시적 해제와 기본 Door Mock은 유지합니다. 관련 복구 단위 시험을 추가했고,
최종 루트 CTest는 전체 실행했습니다. Backend 독립 구성은 재빌드 및 해당 단위 시험만 수행하여
앞선 전체 CTest의 실패를 성공으로 대체하지 않았습니다. 기본 Door Mock 실제 실행은 이 보완 전 기록입니다.
Frontend는 기존 Node로 `react-scripts/scripts/build.js`를 직접 실행해 별도 `frontend-build`에
출력했습니다. npm 설치, D 소스 변경, 브라우저 조작은 하지 않았습니다. D UNKNOWN 표시는 여전히 미구현입니다.

### A-B 및 WebSocket transport 공존

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172 --signal-config signal_processing/config/elevator_1st.cfg --websocket
```

B는 기존과 같이 동기적으로 처리하고 결과를 console에 출력합니다. WebSocket은
별도 I/O thread에서 연결과 요청 수신을 처리하지만 실제 B 결과를 Snapshot으로
변환하지 않습니다. Parser Benchmark의 측정 범위도 변경하지 않습니다.

향후 연결 위치는 다음과 같으며 **아직 구현되지 않았습니다**.

```text
SignalPipeline::process() -> C 처리 결과 -> A JSON 직렬화 -> publishSnapshot()
```

C의 공개 API 확정 전에는 cluster_id를 승객 ID로 쓰거나 승객 상태를 만들지 않습니다.
B의 문 상태 추정값은 실제 문 상태로 확정하지 않으며 상대 µs를 임의의 절대시각으로
변환하지 않습니다. v6의 MANUAL 우선 원칙은 유지하되 현재는 요청 수신만 구현되어
있고 실제 우선 처리와 제어 실행은 없습니다.

### v6 Mock 자동 수신 확인

다음 명령은 별도 PowerShell 두 창에서 실행합니다. 먼저 서버의 listening 로그를 확인합니다.
루트 빌드는 `build-integrated/backend/Release`, 독립 빌드는
`backend/build-websocket-integration/Release`를 사용합니다.

```powershell
# 창 1: 실제 Mock 프로세스 (자동 종료)
.\build-integrated\backend\Release\elevsafe_backend.exe --ws-mock --ws-duration 60
# 창 2: 별도 프로세스에 접속하는 명시적 검증 모드
.\build-integrated\backend\Release\websocket_server_test.exe --case mock-process --diagnostics
```

`mock-process`는 두 클라이언트의 동일 v6 JSON 21개, 증가하는 상대 시간,
20구간 평균 수신/생성 주기, 개별 종료·재접속을 검사합니다. 로컬 테스트의 평균 주기
허용 범위 70~150ms는 스케줄링 여유이며 실시간 보장값이 아닙니다. OPEN/HOLD/CLOSE와
잘못된 요청을 보내며 서버 로그로 수신을 확인합니다. FIFO와 느린 클라이언트 격리는
기존 CTest `websocket_server_test`가 검사합니다. 이 외부 Mock 모드는 CTest에서 자동 실행하지 않습니다.
소켓 실패는 그대로 실패이며 재접속 기능 검사는 실패 재시도와 구분합니다.

2026-11-01 v6 변경 검증에서는 루트/Backend 독립 x64 Release Build와 각 CTest 12/12가
통과했습니다. 별도 Mock 프로세스의 두 클라이언트가 동일 JSON 21개를 수신했고, 20구간
평균 수신 간격 99.71ms, Mock 상대 시간 간격 99.71ms를 관측했습니다. OPEN/HOLD/CLOSE
수신 순서, 잘못된 요청 거부, 개별 종료·재접속 및 서버/클라이언트 종료 코드 0을 확인했습니다.
첫 실행 보조 스크립트의 종료 코드 수집 누락은 별도 로그로 보존하고 수집 방식을 수정해
확인했습니다. 이번 실행에서 10014가 없었지만 기존 간헐 오류의 해결이나 장시간 안정성을
뜻하지 않습니다. 실제 D GUI·C 결과·물리 센서 E2E는 이번 검증에 포함하지 않았습니다.

### A팀 가변 Mock Snapshot 자체 시험

`tests/websocket_mock_scenarios.h`는 기존 Mock JSON에서 시험용 전체 Snapshot을 구성합니다.
제품 `--ws-mock`의 한 명·고정 상태·목표 10Hz 동작은 변경하지 않습니다. 시험 데이터는
C Tracking 또는 실제 문 상태가 아닙니다. `websocket_message_test`와 기존 서버 시험 실행 파일의
`--case mock-scenarios`가 이 데이터를 재사용하며 별도 서버 프로그램은 추가하지 않습니다.

| 순서 / 상대 µs | 승객 목록 | 문 상태 | 확인 목적 |
|---|---|---|---|
| 0 / 0 | 빈 목록 | CLOSED | 최초 시각 0, 승객 없음 |
| 1 / 100000 | 101: OUTSIDE | OPENING | 한 명, 음수 좌표, safety null |
| 2 / 200000 | 101: ENTERING, 202: INSIDE/FALL | OPEN | ID 유지·추가, 좌표 이동, 복수 승객 |
| 3 / 300000 | 101: INSIDE/IMMOBILE, 202: EXITING | CLOSING | 위치·상태·안전 상태 갱신 |
| 4 / 400000 | MOCK-303: OUTSIDE | CLOSED | 전체 교체, 새 문자열 ID, label 생략 |
| 5 / 500000 | 빈 목록 | OPEN | 기존 승객 전체 제거 |

두 client가 각각 받은 text가 발행 원문과 같은지 비교하므로 ID·좌표·상태·목록 및 순서를
함께 검사합니다. 각 Snapshot을 두 client가 수신한 뒤 다음 것을 발행하여 의도된 latest-only
큐 교체와 콘텐츠 보존 검사를 분리합니다. 이 시퀀스의 100000µs 증가는 **시험값**이며
벽시계 10Hz 측정이 아닙니다. 실제 제품 주기는 위 `mock-process` 시험으로 별도 측정합니다.
잘못된 JSON/시간 publish는 false를 반환해야 하며, 이후 유효 Snapshot 수신도 확인합니다.
timestamp 문자열의 날짜 문법이나 서버의 메시지 간 시각 역행 거부는 검사 대상이 아닙니다.

OPEN/HOLD/CLOSE 접수 후 송신 client를 정상 종료하고, `popCommand()`로 같은 client ID와
FIFO 순서를 검사합니다. 이는 현재의 미처리 명령 보존 동작을 확인하며 제어 정책 승인을 뜻하지
않습니다. 큐 포화·잘못된 수동 요청·느린 client·재접속은 기존 `websocket_server_test`를 유지합니다.

```powershell
# OS 할당 임시 포트 사용; 서버와 두 client를 시험이 소유합니다.
& '.\build-integrated\backend\Release\websocket_server_test.exe' --case mock-scenarios --diagnostics
```

CTest에는 `websocket_mock_scenarios_test`로 별도 등록되어 기존 서버 시험 실패로 이 시나리오가
건너뛰어지지 않습니다. 원래 테스트는 제외하지 않으며 소켓 오류는 실제 코드·category·단계와
함께 FAIL로 남깁니다. D 화면 표시, 브라우저의 빈 목록 처리·경보 동작, 실제 C/문 제어는 별도 검증입니다.

### A팀 자체 검증 결과 (2026-11-05)

기준 HEAD `30dbbe37`에서 테스트·문서만 확장하고, MSVC 19.44 / Windows SDK 10.0.26100.0 /
VS 2022 x64 Release로 별도 Build했습니다. Boost 1.86.0 및 JSON 3.11.3의 기존 소스를 재사용했습니다.

| 검증 | 결과 |
|---|---|
| 루트 Release Build / CTest | PASS / 17/17 PASS (43.75초) |
| Backend 독립 Release Build / CTest | PASS / 17/17 PASS (41.55초) |
| 가변 Mock 6개·두 client 동일 수신·잘못된 publish 9개 거부 | 양쪽 PASS |
| 명령 FIFO·포화·종료 후 보존·기존 느린 client/재접속 | 양쪽 PASS |
| 기존 A–B·Background ON/OFF·Radar 재연결 자동 회귀 | 양쪽 PASS |
| 별도 제품 Mock 20초 / 기존 mock-process client | 양쪽 ExitCode 0 |
| 동일 Snapshot 수신 | 두 client 각각 21개, 20구간 비교 |
| 평균 수신 간격 / Mock 상대시각 간격 | 99.5142ms / 99.5085ms |
| 제품 Mock 수동 요청 | OPEN/HOLD/CLOSE 3개 접수, 잘못된 요청 1개 거부 |

두 CTest는 각각 한 번 순차 실행했고, 제품 Mock 시험도 한 번 실행했습니다. 이번 실행에서는
Winsock 10014가 재현되지 않았으나 기존 간헐 오류의 해결이나 장시간 안정성을 의미하지 않습니다.
처음 sandbox 내부 병렬 MSBuild는 컴파일 전에 작업자 기동이 진행되지 않아 해당 프로세스만
정리했고, 일반 사용자 토큰에서 단일 작업자(`/nr:false`)로 Build했습니다. 최초 로그도 보존했습니다.
테스트의 소켓 설정·backlog·재시도 정책과 제품 서버·JSON·Mock 코드는 변경하지 않았습니다.

격리 출력과 원시 로그: `build-integrated/diagnostics/ws-mock-ready-20261105_190551/`.
`root-ctest.log`, `backend-ctest.log`, 각 Build의 `Testing/Temporary/LastTest.log`,
`mock-process-result.json`, `mock-server-*.log`, `mock-client-*.log`에 결과를 보존합니다.
PID·실행 시각·EXE SHA256·ExitCode는 `mock-process-result.json`에 기록했습니다.

이번 검증 EXE를 D팀이 동일 PC에서 실행하는 명령:

```powershell
Set-Location 'C:\Users\kimji\ElevSafe'
$bin = 'C:\Users\kimji\ElevSafe\build-integrated\diagnostics\ws-mock-ready-20261105_190551\root\backend\Release'
& "$bin\elevsafe_backend.exe" --ws-mock --ws-bind 127.0.0.1 --ws-port 8080 --ws-duration 60
```

연결 URL은 `ws://127.0.0.1:8080/radar`입니다. 제품 Mock은 계속 고정 승객 1명을 발행합니다.
가변 시나리오는 별도 CTest 내부 시험이며 브라우저용 시나리오 서버를 추가하지 않았습니다.
Frontend는 수정·실행하지 않았고 실제 A–D 화면 연동, C Tracking, 문 제어, LAN 공개 및 센서
시험은 수행하지 않았습니다. D 연결 활성화 후 표시·입력·연결 중단 처리를 공동 검증해야 합니다.

### AWS와 실제 제어 연동 범위

현재 A Backend에 AWS 클라이언트/전송 경로는 없습니다. 따라서 AWS 장애 격리가
구현·검증됐다고 주장하지 않습니다. 향후 C 결과로 만든 Snapshot의 별도 소비 지점에
클라우드 전송을 연결하되, 제한된 별도 큐·worker·timeout과 오류 진단을 두어야 합니다.
Radar 수신/B 처리, 로컬 제어 및 `publishSnapshot()`에서 클라우드 응답을 기다리거나
클라우드 실패로 로컬 파이프라인을 종료하지 않도록 설계해야 합니다. 이는 후속 설계 조건이며
이번에는 구현하지 않았습니다. 실제 C/하드웨어 Door Control도 아직 연결하지 않았습니다.

### Radar Emulator TCP 비교 모드

기존 Radar Emulator는 최초 활성화 위치가 무작위이고 PCR 원본 frame ID를 stream 번호로
바꿉니다. `Restart from beginning`은 끝에 도달한 뒤의 반복 시작점만 0으로 바꾸며 단발 재생은
제공하지 않습니다. 아래 모드는 전체 point의 순서·float32/int32 비트로 논리 프레임을 찾습니다.
FNV-1a 지문은 후보 검색에만 사용하고 전체 값을 다시 대조합니다. 기준 지문 중복/충돌은
시작 전에 거부하며, 다른 데이터로 조용히 재동기화하지 않습니다.

Emulator에서 S0-3 파일을 Default record로 선택하고 Loop mode를 `Restart from beginning`으로
설정하세요. Launchpad/키보드로 파일을 전환하거나 창을 최소화하지 마세요. 서버가 HTTP 8000과
TCP 29172를 정상 개방한 다음, ElevSafe 루트에서 새 출력 디렉터리를 지정합니다.

```powershell
$base = '.\backend\build\s0-3-b-baseline-20261031_014816'
$out = ".\backend\build\s0-3-emulator-e2e-$(Get-Date -Format 'yyyyMMdd_HHmmss')"
.\backend\build-websocket-integration\Release\signal_replay_compare.exe --tcp 127.0.0.1 29172 `
  --config signal_processing/config/elevator_1st.cfg `
  --input "$base\S0-3_t01.csv" --baseline "$base\b_frames.csv" `
  --output $out --sync-timeout 120 --total-timeout 240 --s0-3
$LASTEXITCODE
Get-Content "$out\tcp_comparison_summary.json"
```

입력 CSV와 기준 결과는 Git에 포함되지 않습니다. 기존 검증 자료를 확보한 뒤 `$base`를
해당 폴더로 지정하세요. 출력은 실행마다 새 경로를 사용하고 기존 결과는 보존합니다.

timeout은 1~86400초이며 기본값은 동기화 120초, 전체 240초입니다. 두 제한 모두 TCP 연결
시작부터 계산합니다. 제한을 보장하기 위해 숫자 IPv4/IPv6 주소만 허용하며 DNS 조회는 하지 않습니다.
일반 Backend의 `RetinaClient::run(log)`는 기존 blocking 동작을 유지합니다. 검증 도구만 deadline
오버로드에서 같은 수신 thread의 non-blocking socket/select를 사용하며 별도 watchdog thread는 없습니다.

초기 꼬리 구간은 B에 넣지 않습니다. 원본 index 0 확인 시 새 B 객체를 생성하고 이후 한 순환을
연속 비교합니다. 수신 RadarFrame을 기존 BackendPipeline/Adapter로 전달하고 stream ID와 host
timestamp가 B까지 보존되는지 검사합니다. 원본 ID/PCR 시간은 매핑용이며 수신 값을 덮어쓰지 않습니다.
클러스터/문 상태 비교에서만 논리 ID로 대응합니다. PCR 시간과 host 시간의 동일성을 요구하지 않습니다.

### Background ON TCP 검증

`--require-background` 지정 시 TCP 연결 전에 같은 입력/설정으로 **실제 ON인 B 직접 처리**를
한 번 순차 실행해 기준을 생성합니다. 기존 OFF `b_frames.csv`를 재사용하지 않도록 이 모드에서는
`--baseline`을 거부합니다. 생략 시 기존 OFF CLI와 외부 baseline 비교 동작을 유지합니다.

```powershell
$base = '.\backend\build\s0-3-b-baseline-20261031_014816'
$out = ".\backend\build-websocket-integration\validation\s0-3-bg-on-tcp-$(Get-Date -Format 'yyyyMMdd_HHmmss')"
.\backend\build-websocket-integration\Release\signal_replay_compare.exe --tcp 127.0.0.1 29172 `
  --config signal_processing/config/elevator_1st.cfg --input "$base\S0-3_t01.csv" `
  --output $out --require-background --s0-3 --sync-timeout 120 --total-timeout 240
$LASTEXITCODE
Get-Content "$out\tcp_comparison_summary.json"
```

새 기준은 `tcp_reference_frames.csv`에 저장합니다.
기준 측 `background_direct`와 논리 index 0에서 초기화한 `background_live`는 실제
`usesBackground()` 값입니다. 어느 쪽이든 요구 모드와 다르면 실패하며, live 미생성 시 `null`입니다.
기존 `background_enabled`는 기준 측 실제 값을 기록합니다.
`baseline_source=generated_background_on`, `external_baseline_compared=false`는 새 ON 기준을 뜻합니다.

`background_stats_direct`는 입력 전체, `background_stats_live`는 동기화 후 B에 전달한 구간만의
`input_points`, `removed_points`, `removal_rate_percent`, `processed_frames`를 기록합니다.
제거율 분모는 ROI 통과 점이며 입력 0이면 `null`입니다. 동기화 전 제외 frame은 live 통계에 섞이지 않습니다.
오프라인에서는 대응 항목을 `background_stats_direct` / `background_stats_adapter`에 기록합니다.

현재 설정과 제공된 맵으로 수행한 S0-3 ON 오프라인 비교에서는 양쪽 실제 ON,
1,673 frame / 381,730 point / 333 cluster / OPEN 1,175 / 전환 7, 모든 비교 차이 0을 확인했습니다.
두 경로의 배경 입력은 ROI 통과 점 8,829개, 제거는 0개(0%)였습니다. 기존 OFF 기준 결과와의
오프라인 회귀도 통과했으며, 이 결과는 실제 TCP/Emulator ON 검증과 구분합니다.

S0-3은 ROI에서 정적 배경이 제거되어 ON이어도 제거율 0%가 정상일 수 있습니다.
ON/OFF 출력 일치를 배경 제거 기능의 효과 검증으로 취급하지 않으며, 다른 시나리오의 효과 검증과 구분합니다.
사용자가 격리된 backlog 1 Radar Emulator GUI로 수행한 ON TCP E2E 결과도 확인했습니다.
수신 3,137 frame 중 동기화 전 1,464개를 제외하고 1,673 frame / 381,730 point를 검증했으며,
직접/live 모두 실제 ON, cluster 333개, Parser 거부 및 point/frame/cluster/문 상태 차이는 모두 0이었습니다.
`complete=true`, `passed=true`, `termination_reason=complete`, `error_code=0`으로 완료됐습니다.
기록은 `backend/build-websocket-integration/validation/s0-3-bg-on-tcp-20261101_113453/`에 보존합니다.
이는 격리된 Emulator 실행 파일의 사용자 검증이며 정식 소스 반영 후 GUI를 새로 검증한 결과나
물리 RETINA-4SN 검증은 아닙니다. Winsock 10014의 근본 원인도 해결됐다고 판단하지 않습니다.
기존 OFF 기록과 구분하고, 다음 실행의 동기화 전 제외 수를 470 또는 1,464로 고정하지 마세요.

마지막 논리 프레임까지 비교하면 동일 thread callback에서 `stop()`을 호출합니다. join/detach는 없고,
같은 feed의 추가 callback은 완료 결과를 변경하지 않습니다. `received_tcp_frames`는 완료 전 callback
수이며 Parser 통계는 제외 구간과 같은 feed의 추가 packet까지 포함한 연결 전체 통계입니다.
부분 수신·EOF·동기화/전체 timeout·데이터/B 결과 차이·소켓 오류는 실패 종료 코드 1로 기록합니다.
Parser 거부 packet이 있는 실행도 PASS로 처리하지 않습니다. 연결 재시도는 하지 않습니다.
`complete`는 한 순환 비교 완료, `passed`는 Parser 거부 여부까지 포함한 최종 판정입니다.
`processed_frames`에는 비교 중 실패한 frame도 포함될 수 있고 `verified_frames`는 일치한 frame 수입니다.
차이 개수는 처음 실패하기까지 발견한 차이 기록 수이며 실패 후 나머지 데이터를 계속 비교하지 않습니다.

출력은 `tcp_frame_mapping.csv`, `tcp_frames.csv`, `tcp_frame_diff.csv`, `tcp_point_diff.csv`,
`tcp_comparison_summary.json`, `tcp_stdout.log`, `tcp_stderr.log`입니다. 매핑에는 논리 index,
원본/stream ID, PCR/host 시간, point 수와 일치 여부를 남깁니다. 초기화 실패 시에는 오류 요약과
stderr만 남을 수 있습니다. 숫자 오류 코드 10014는 환경 진단 대상으로 별도 확인하세요.
Emulator가 Listen에 실패한 원인은 클라이언트 연결 실패만으로 확정할 수 없으므로 GUI 로그도 확보해야 합니다.

자가 테스트는 매핑 로직과 실제 Parser의 one-byte/multiple packet 입력에서 BackendPipeline/B까지
검증합니다. 이미 만료된 deadline의 RetinaClient 경로도 검사하지만 실제 TCP/Emulator 성공을 뜻하지는
않습니다. 장비/GUI 실행 결과는 별도 확인해야 하며, 기존 오프라인 비교 성공을 E2E 성공으로 간주하지 않습니다.

추가 로컬 소켓 fixture에서 기존 RetinaClient 경로의 정상 완료·추가 packet 무시·조기 EOF·truncated
packet·동기화/전체 timeout을 확인했습니다. 별도 연결 실패 시험에서는 Winsock 10014도 관찰되었습니다.
이는 작은 테스트 입력에 대한 검증이며 S0-3 전체 Radar Emulator E2E 성공을 뜻하지 않습니다.
별도로 완료한 S0-3 전체 Emulator TCP E2E 결과는 아래 검증 상태에 기록합니다.

## 검증 상태

### A-B 및 WebSocket 공존 검증 (2026-10-31)

통합 코드의 Windows x64 Release Build와 전체 CTest 11/11이 통과했습니다.
S0-3(1,673 frame / 381,730 point) 및 front_man(164 frame / 106,024 point)을
통합 비교 실행기로 다시 처리했으며 프레임별 결과와 cluster 상세 float32 비트 비교의
불일치는 모두 0건입니다. 두 경로 모두 배경 맵 비활성 조건입니다.

별도 Mock 프로세스에 실제 WebSocket client 2개를 연결해 JSON 수신, 수동 요청
OPEN/HOLD/CLOSE 수신 및 연결 해제·프로세스 정상 종료를 확인했습니다.
`--ws-mock`과 별칭 `--mock-websocket` 모두 검사했으며 명령 실행이나 ACK는 없습니다.

| 통합 바이너리 반복 검증 | 성공 | Winsock 10014 실패 |
| --- | ---: | ---: |
| 전체 서버 테스트 20회 | 12 | 8 |
| slow-client 단독 10회 | 5 | 5 |
| Mock 시작/종료 20회 | 17 | 3 |

이 반복 실행에서 10048, Timeout 및 기타 오류는 없었습니다. 별도 .NET IPv4 loopback
bind/listen(포트 0, backlog 8) 20회는 모두 성공했지만 조건이 다른 단일 기준선이므로
과거 독립 TCP의 10014 재현을 반박하거나 원인을 확정하는 결과는 아닙니다.
10014를 무시하거나 자동 재시도로 성공 처리하지 않았으며 반복 안정성은 미해결입니다.
기록은 Git 제외 경로 `backend/build-websocket-integration/validation/`에 보관합니다.

이번 통합 코드로 Radar Emulator TCP E2E, 실제 물리 센서 및 D Frontend GUI는
재검증하지 않았습니다. 아래의 기존 S0-3 TCP E2E 기록과 구분하세요.

기존 A 수신·파싱 범위에서 Radar Emulator에 대해 다음을 확인했습니다.

- TCP `29172` 연결 성공
- Emulator Clients `0 -> 1` 확인
- `.pcr` 데이터에서 `frameCount` 연속 증가 확인
- `pointCount` 정상 출력
- `x/y/z/doppler/power/targetId` 정상 파싱 확인

실제 RETINA-4SN에 대해서는 AP Mode 및 TCP `29172` 연결 후
frame 연속 수신과 parsing을 확인했습니다. 이는 B까지 포함한 물리 센서 E2E 검증은 아닙니다.

### 오프라인 B 및 A-B Adapter 비교

기존 `front_man.pcr`을
별도 CSV로 변환한 164 frame / 106,024 point에서 B 직접 입력과 Adapter 경로의
메타데이터·문 상태·cluster 결과가 일치했습니다. 이 비교는 외부 배경 맵 없는 테스트
설정을 사용했습니다.

S0-3 로컬 PCR 변환 CSV(1,673 frame / 381,730 point)를 기존 `elevator_1st.cfg`와
배경 제거 비활성 조건에서 처리했습니다. B 단독 실행기로 아래 집계값을 재현한 뒤,
동일 입력의 B 직접 경로와 BackendPipeline/Adapter 경로를 비교했습니다.
두 경로는 기존 로컬 B 기준 CSV의 모든 frame 결과와 일치했고, cluster 333개의 상세 필드도
float32 비트 단위로 일치했습니다.
OPEN 1,175 / CLOSED 489 / UNKNOWN 9 / 전환 7회이며 누락·중복·순서 및 timestamp 차이는
없었습니다. 이 단계는 TCP 수신을 포함하지 않는 오프라인 비교입니다.

### S0-3 전체 Radar Emulator TCP E2E PASS

2026-10-31 Windows PowerShell에서 실제 Radar Emulator로 실행한 결과를 확인했습니다.
검증 입력은 Session 1개의 `S0-3_t01.csv`(1,673 frame / 381,730 point)이며,
기존 `signal_processing/config/elevator_1st.cfg`, `door_state.close_threshold = 0.5`, 배경 맵 비활성 조건을 사용했습니다.

```text
Radar Emulator -> TCP 29172 -> RetinaClient -> RetinaStreamParser -> RadarFrame
  -> BackendPipeline::onFrame() -> FrameBuffer -> BackendPipeline::drain()
  -> SignalAdapter (toSignalFrame) -> SignalPipeline
```

Emulator가 원본 frame ID를 stream ID로 바꾸므로 전체 point 데이터로 논리 프레임을
대응시켰습니다. 수신 2,143 frame 중 처음 470 frame은 동기화 이전 구간으로 제외했고,
원본 index 0~1672의 완전한 한 순환 1,673 frame을 검증했습니다.
해당 구간의 TCP stream ID는 1517~3189이며 원본 ID와 같은 값으로 취급하지 않습니다.

| 항목 | 결과 |
| --- | ---: |
| 검증/처리 frame | 1,673 / 1,673 |
| 검증 point | 381,730 |
| cluster 합계 / 상세 비교 | 333 / 333 |
| OPEN / CLOSED / UNKNOWN | 1,175 / 489 / 9 |
| 문 상태 전환 | 7 |
| point / frame / cluster / 문 상태 불일치 | 모두 0 |
| Parser 처리 / 거부 packet | 2,143 / 0 |
| Parser 잔여 byte | 0 |

종료 코드 0, `complete=true`, `passed=true`, `termination_reason=complete`, `error_code=0`으로
정상 완료했습니다. 프레임 매핑과 결과 CSV도 기존 로컬 B 기준 결과와 대조했습니다.
검증 기록은 로컬 `backend/build/s0-3-emulator-e2e-20261031_131708/`에 보존하며 Git에는 포함하지 않습니다.

### 남은 제한 사항

- 이번 전체 E2E는 Radar Emulator 기반이며 실제 RETINA-4SN 물리 센서의 A-B E2E 검증은 아닙니다.
- Windows Winsock 10014의 근본 원인은 미확정입니다. 이번 1회 성공이 반복 연결 안정성을 보장하지 않습니다.
- 기준은 로컬 B 단독 재현 결과이며 B 팀원이 생성한 프레임별 원본 결과와 직접 비교한 것은 아닙니다.
- C/D 시스템 통합과 실제 현장 승객 상태 인식 정확성은 이번 검증 범위 밖입니다.

## 실행

### Radar Emulator

Radar Emulator를 실행하고 `.pcr` 데이터셋을 연 뒤 재생을 시작합니다.

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172
```

### RETINA-4SN

Device Discovery 없이 장치 IP를 직접 지정합니다.

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 192.168.30.1 29172
```

실제 RETINA-4SN에서는 AP Mode 및 TCP `29172` 연결 후
`frameCount`, `pointCount`, `x/y/z/doppler/power/targetId`의 연속
수신 및 parsing까지 검증했습니다. 실제 연결 가능 여부와 데이터 수신은
PC IP 설정 및 Radar의 Client 접근 허용 IP 설정에 영향을 받을 수 있습니다.

host와 port는 command line으로 지정할 수 있습니다. port는 `1`부터
`65535`까지 허용됩니다. host를 생략하면 `main.cpp`에서
`127.0.0.1`을 사용하고, port를 생략하면 `29172`를 사용합니다.

### B Signal Processing 활성화

프로젝트 루트 기준 예시입니다. 선택한 설정은 실제 센서 설치 조건에 맞아야 합니다.
아래 minitest 설정은 센서 좌표를 유지하는 비엘리베이터 테스트용입니다.

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172 --signal-config signal_processing/config/minitest.cfg
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 192.168.30.1 29172 --signal-config signal_processing/config/elevator_1st.cfg
```

설정 경로는 명시적으로 한 번만 지정하며 기본 설정을 자동 선택하지 않습니다.
옵션은 기존 host/port 뒤에 둡니다. 설정 내부 배경 맵 상대 경로는 B의
`Config::getPath()`가 설정 파일 폴더 기준으로 해석합니다.

```text
signal frame_id=7 timestamp_us=50000 inputPoints=12 clusters=1 estimated_door_state=UNKNOWN
```

B의 기존 오프라인 실행기도 독립적으로 빌드할 수 있습니다.

```powershell
cmake -S signal_processing -B signal_processing/build
cmake --build signal_processing/build --config Release
```

### Parser Latency Benchmark

Radar Emulator로 PCR을 한 번 재생하며 A 수신/파싱 구간을 측정하려면 다음처럼
`--benchmark`를 마지막 인자로 지정합니다.

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172 --benchmark
```

Radar Emulator의 loop playback에서 첫 N개의 valid frame만 측정하려면
`--benchmark-frames N`을 함께 지정합니다. 예를 들어 164-frame PCR의 공식 측정은
다음과 같습니다.

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172 --benchmark --benchmark-frames 164
```

이 모드는 기존 frame별 console 출력을 생략하지만, 동일한 synchronous
`BackendPipeline` enqueue/drain 경로는 유지합니다. `RetinaStreamParser`가 valid
`RadarFrame`을 완성하기 전의 parser core latency만 기록하므로 `recv()` 대기,
network/replay interval, `timestampUs` 설정, pipeline 및 console I/O는 측정값에
포함되지 않습니다. 처음 5개의 valid frame은 warm-up으로 제외하며 이후 frame의
average, nearest-rank p95, maximum을 milliseconds 단위로 출력합니다. frame limit에
도달하면 summary를 한 번 출력하고 이후 반복 frame은 parsing을 계속하되 benchmark
통계에는 추가하지 않습니다.

`--signal-config`를 함께 지정하면 B 처리도 수행하지만 frame별 B 로그는 생략합니다.
B 처리시간은 Parser Benchmark에 포함되지 않습니다. 다만 같은 thread의 B 처리는
다음 recv 및 실행 scheduling에 영향을 주므로 기존 parser-only 비교에는 B 옵션을 생략하세요.

### 선택적 Radar TCP 재연결

기본 실행은 한 연결만 처리하며 정상 EOF는 0, 연결·수신 실패는 1로 종료합니다.
`--radar-reconnect`를 명시한 경우에만 다음 정책을 적용합니다. 기존 TCP 정합성 검증 도구
`signal_replay_compare --tcp`의 단일 세션 동작에는 적용하지 않습니다.

```powershell
.\build-integrated\backend\Release\elevsafe_backend.exe 127.0.0.1 29172 `
  --signal-config signal_processing/config/elevator_1st.cfg --websocket --radar-reconnect
# 정상 EOF까지 자동 복구하려는 경우에만 --radar-reconnect-eof를 추가합니다.
```

- 최초 연결 실패 또는 연결 거부/reset/aborted/timeout/net-down/reset/unreachable/host-unreachable
  오류 중 선정한 native 코드만 복구 대상으로 삼습니다. resolve·설정·초기화·처리 예외와
  Winsock 10014/10048 등 다른 오류는 재시도하지 않습니다. 실제 stage/code는 보존합니다.
- 최초 시도 외에 **프로세스 전체 최대 3회**, 매회 1초 대기합니다. 연결 성공으로 횟수를
  초기화하지 않습니다. 한도 소진은 종료 코드 1이며 성공할 때까지 반복하지 않습니다.
- 개별 연결은 nonblocking connect와 select/SO_ERROR로 3초 기한을 적용합니다.
  DNS의 무제한 대기를 피하려고 이 모드는 숫자 IPv4/IPv6만 받으며 hostname은 실패합니다.
  기존 기본 모드의 host 처리와 blocking 동작은 유지합니다.
- 복구 예산은 단절 이후 15초이며 대기·세션 재초기화·연결 시도에 적용합니다. 연결 제한은
  남은 복구 예산과 3초 중 작은 값입니다. 연결에 성공한 수신 시간은 복구 예산에 포함하지
  않고 다음 단절 때 새 15초 예산을 시작하지만, 전체 재시도 횟수는 그대로 유지합니다.
- 기한은 steady clock으로 다음 연결 및 대기 진행 시 검사합니다. Windows scheduling,
  동기 설정/배경맵 파일 I/O, B 처리 및 기존 WebSocket 종료를 강제로 선점하지는 못하므로
  전체 프로세스가 정확히 15초 내 종료한다는 hard wall-clock 보장은 아닙니다.
- 정상 EOF는 이 모드에서도 기본적으로 코드 0 종료입니다. `--radar-reconnect-eof`는
  `--radar-reconnect`와 함께만 사용할 수 있으며, EOF 복구도 동일한 전체 3회 한도를 씁니다.
- 재연결 모드에서 Ctrl+C(SIGINT)/SIGTERM은 lock-free 종료 플래그만 설정합니다.
  소켓 정리와 callback 중단은 owner thread에서 수행합니다. select 중에는 최대 100ms,
  재시도 대기 중에는 최대 50ms 간격으로 확인하며 명시적 취소는 코드 0으로 종료합니다.
  진행 중인 B 처리·파일 I/O의 즉시 취소나 강제 종료 시 통계 출력은 보장하지 않습니다.
- **무응답 TCP의 수신 idle timeout/heartbeat는 구현하지 않았습니다.** 연결 후 데이터가
  없어도 계속 기다리지만 재연결 모드의 명시적 취소 확인은 유지됩니다.

매 연결 시도마다 새 RetinaClient·Parser·RelativeFrameClock·BackendPipeline(용량 1)을
사용합니다. 같은 Config로 새 SignalPipeline을 생성해 문 추정 이력과 누적 통계를 초기화하고
배경맵을 다시 로드합니다. 최초 실행과 실제 `usesBackground()`가 달라지면 실패 종료합니다.
첫 유효 프레임의 `timestamp_us`는 다시 0이며 frameCount는 원본 그대로 전달합니다.
이전 연결의 partial packet은 buffered byte 수만 기록한 뒤 버리고 다음 연결로 넘기지 않습니다.

`--ab-profile`/Parser Benchmark도 시도별 새 샘플·warm-up·ID 상태로 집계합니다.
재연결 모드의 `AB_PROFILE_JSON.session_id`는 연결 시도 번호이며, 연결에 실패한 시도는
프레임 0개로 기록될 수 있습니다. 세션 로그의 connected/stage/parsed/buffered/B_frames와
함께 판단해야 합니다. 네트워크 복구 대기는 프레임 성능 window에 합치지 않습니다.

WebSocket은 Radar 재연결 루프 밖에 유지하며 transport/JSON/FIFO는 바꾸지 않습니다.
현재 실제 C Snapshot·문 제어는 없으며 Radar 단절 중 데이터 최신성·수동 명령 만료나
C Tracking ID 복구 정책도 구현하지 않았습니다. 새 Radar 세션을 D에 알리는 JSON 규격은
임의로 추가하지 않았으므로 실제 C/D 연결 전 별도 합의가 필요합니다.

2026-11-01 Windows x64 Release에서 루트·Backend 독립 Build를 통과했습니다.
각각 전체 CTest는 15/16이었으며 루트는 기존 `websocket_server_test`의 client.connect,
Backend는 기존 `websocket_cli_test`의 Radar connect에서 Winsock 10014로 실패했습니다.
두 빌드 모두 신규 재연결 단위·CLI 테스트는 통과했습니다. 기존 Make-Packet을 재사용하고,
서버 재시작은 격리 fixture 프로세스의 종료를 확인한 뒤 같은 포트에 새 프로세스를 띄워
검증했습니다. 10-byte partial packet 폐기, 다음 연결 timestamp 0, B CLOSED → 새 세션
UNKNOWN 및 B_frames 1, 세션별 warm-up과 같은 WebSocket 연결의 명령 수신을 확인했습니다.
이 TCP fixture는 Background OFF이며 실제 Emulator/물리 센서 재시작 검증은 미실시입니다.
기존 Background ON/OFF 자동 회귀와 Mock 두 클라이언트의 동일 JSON 21개 수신·명령 순서·
오류 요청 거부도 통과했습니다. Mock 평균 source 간격은 100.246ms였습니다.

기한 만료의 사전 차단·재시도 예산과 실제 무응답 수신 중 취소는 자동 검증했습니다.
SYN 무응답을 강제한 연결 3초 timeout 실측 및 콘솔 Ctrl+C의 모든 단계별 실행 시험은
아직 수행하지 않았습니다. 이전 fixture 실패와 모든 10014 로그는 덮어쓰지 않았으며 이번
기록은 `build-integrated/diagnostics/radar-reconnect-20261101_215129/`에 보존했습니다.

### A-B 처리 성능 계측

`--ab-profile`은 실제 Radar callback의 동기 enqueue/drain·Adapter·B 처리 시간을
`steady_clock`으로 측정합니다. `--signal-config`가 필수이며 Mock 또는 기존 Parser 전용
`--benchmark`와 함께 사용할 수 없습니다. 기본 실행과 Parser Benchmark의 의미는 유지합니다.

```powershell
.\build-integrated\backend\Release\elevsafe_backend.exe 127.0.0.1 29172 `
  --signal-config signal_processing/config/elevator_1st.cfg --ab-profile `
  --ab-profile-max-frames 10000 --ab-profile-warmup 5
```

최초 5개 callback은 통계만 제외하며 B 처리·문 상태 이력은 계속 갱신합니다.
warm-up은 `0~100000`, 저장 상한은 `1~100000`이며 기본 상한은 warm-up 이후 10000개입니다.
상한 이후에도 Radar/B 처리는 계속하고 전체 실행의 frame/point·오류·ID 카운터는 갱신합니다.
저장 생략 수는 `omitted_after_limit`입니다. 프레임별 정상 console 출력은 억제하며,
통계 배열은 수신 전에 reserve하고 정렬·집계·출력은 실행 종료 후에만 수행합니다.

`AB_PROFILE_JSON `으로 시작하는 한 줄에 다음을 출력합니다. 분위수는 nearest-rank
`ceil(p*N)-1`이며 50ms 초과는 정확히 50ms인 샘플을 포함하지 않습니다.

- `adapter_ms`: `toSignalFrame()` 전후
- `b_ms`: `SignalPipeline::process()`와 반환값 수신 전후
- `ab_span_ms`: Adapter 시작부터 B 반환까지 (context 준비와 중간 계측 포함)
- `pipeline_ms`: enqueue 전부터 drain 반환까지 (프레임 복사·소비·객체 정리 포함)
- `callback_work_ms`: callback 초반부터 종료 직전까지, 통계 저장 자체는 제외
- `callback_interval_ms`: 저장한 연속 callback의 진입 간격, 수신/센서 송신 간격과 구분

처리 시간 분포는 성공한 저장 샘플만 사용합니다. 전체 실행의 실패 프레임 수와 저장 생략은
별도로 기록하므로 성공 샘플만 보고 완전한 실행으로 판정하면 안 됩니다. window FPS/points/s는
첫 저장 callback 진입부터 마지막 저장 callback 종료까지의 wall time과 그 구간의 성공 처리량입니다.
이 값은 초기화·TCP 연결 대기와 warm-up을 제외하고 callback 사이 수신 공백은 포함합니다.
`clean_eof`, `run_returned`, 종료 코드, 실패 프레임, Parser 통계도 함께 확인합니다.
예외로 run이 반환되지 않으면 Parser 통계는 미완료/null이며, 강제 종료·Ctrl+C 중단은 JSON을
남기지 못할 수 있습니다. 측정 후 Emulator의 서버를 정상 중지해 TCP EOF를 전달하세요.

ID 불연속/인접 중복/전방 gap 추정/역행·reset은 전체 실행, 비인접 중복 ID는 저장 window에서
집계합니다. uint32 정상 wrap은 누락으로 세지 않습니다. FrameBuffer 교체는 기존 단일 스레드·
용량 1 조건에서 enqueue 직전 pending 크기로 관측합니다. 교체 0은 OS/TCP/송신 큐의
대기 0을 뜻하지 않습니다. 센서 송신 시각이 없어 end-to-end 절대 지연을 측정하지 않습니다.
계측 clock 호출과 bookkeeping도 실행 시간에 영향을 주며 hard real-time 보장은 아닙니다.

오프라인 수치는 기존 B 직접 처리/Adapter 동등성 테스트를 재사용해 얻을 수 있습니다.

```powershell
.\build-integrated\backend\Release\signal_integration_test.exe --ab-profile `
  signal_processing/config/elevator_1st.cfg `
  backend/build/s0-3-b-baseline-20261031_014816/S0-3_t01.csv
```

오프라인 JSON의 `mode=offline_equivalence`와 실제 `background_enabled`를 확인합니다.
Adapter/B/pipeline 구간 밖에서 입력 재구성과 B 직접 기준·결과 비교를 수행하므로 오프라인
wall FPS에는 이 비용도 포함됩니다. 오프라인 callback 간격은 제공하지 않습니다.
이 실행은 CSV의 처음부터 끝까지 처리하지만 실제 TCP/Emulator 성능을 뜻하지 않습니다.
오프라인 `callback_work_ms`도 실제 네트워크 callback이 아닌 재생 계측 scope의 시간입니다.
현재 TCP profile은 연결 후 들어오는 프레임을 측정하며 S0-3 원본 index 0 동기화 기능은 없습니다.
전체 논리 한 주기의 정합성은 기존 `signal_replay_compare --tcp`로 별도 확인해야 합니다.
Background ON/OFF는 실제 설정·맵 로드 상태로 구분하고 기존 파일을 덮어쓰지 마세요.

2026-11-01 Windows x64 Release의 S0-3 오프라인 측정은 ON/OFF 모두 1,673프레임·
381,730포인트를 처리하고 B 직접 기준과 동등성 검사를 통과했습니다. 각 측정에서 warm-up
5개를 통계만 제외해 1,668개를 집계했습니다. 원본 설정·배경맵은 유지하고 OFF만 진단 폴더의
설정 사본에서 map 경로를 비웠으며 실제 `background_enabled`를 확인했습니다.

| 조건 / 구간 (ms) | 평균 | P50 | P95 | P99 | 최대 |
| --- | ---: | ---: | ---: | ---: | ---: |
| ON / Adapter | 0.001952 | 0.001800 | 0.004100 | 0.005400 | 0.008500 |
| ON / B | 0.010342 | 0.005600 | 0.034500 | 0.076500 | 0.241100 |
| ON / enqueue~drain | 0.012775 | 0.008100 | 0.038500 | 0.080600 | 0.247900 |
| OFF / Adapter | 0.001868 | 0.001700 | 0.003800 | 0.004900 | 0.009700 |
| OFF / B | 0.009597 | 0.005600 | 0.031500 | 0.070400 | 0.160800 |
| OFF / enqueue~drain | 0.011938 | 0.008000 | 0.034800 | 0.073100 | 0.164100 |

측정한 위 구간의 50ms 초과는 모두 0건이며 FrameBuffer 교체·중복도 0건이었습니다.
ID 점프 86회(전방 gap 추정 242)는 입력 CSV 자체와 같으며 Adapter가 새로 누락시킨
프레임 수로 해석하지 않습니다. 이 데이터는 ROI 이후 부하가 작고 계측 오버헤드도 포함하므로
실제 센서·고밀도 장면·장시간 TCP 처리의 지연 상한을 보장하지 않습니다.
이 수치는 CTest와 겹치지 않게 각 모드를 한 번 실행한 결과입니다. 오프라인 원시 로그/JSON은 Git 제외 경로
`build-integrated/diagnostics/ab-profile-20261101_210700/`에 보존했습니다.

별도의 사용자 수동 TCP 계측에서는 Emulator S0-3 약 20 FPS, Background ON으로
3,418프레임·770,463포인트를 처리했습니다. 측정 window 처리율은 20.0065 FPS였으며
B 처리 시간은 평균 0.026453ms, P95 0.082500ms, P99 0.169700ms, 최대 0.364200ms였습니다.
Parser 거부·처리 실패·FrameBuffer 교체는 모두 0건이었고 정상 TCP EOF, 종료 코드 0을
확인했습니다. 이 실행은 원본 논리 프레임 0에 동기화한 한 주기 검증이 아니므로 기존
S0-3 전체 정합성 검증과 구분합니다. 버퍼 교체 0건이 네트워크 대기나 절대 지연 0을 뜻하지 않습니다.

계측 구현 검증에서는 루트·Backend 독립 Release Build가 통과했고, 루트 CTest는 14/14,
Backend 독립 CTest는 12/14가 통과했습니다. Backend의 WebSocket 서버·CLI 테스트
2건에서는 기존 Winsock 10014가 재현됐으며, 이 문제의 해결을 의미하지 않습니다.

## 현재 구현 범위

- Retina-4SN / Radar Emulator TCP client
- TCP `29172` direct connection
- raw binary stream 수신
- stream buffer 기반 packet boundary 복원
- partial `recv` 및 여러 packet 처리
- packet magic과 `packageSize` 검증
- Radar Frame parsing
- point cloud parsing
- `RadarPoint` 및 `RadarFrame` 자료형
- connection-relative `timestampUs` 생성 및 전달
- parser core latency benchmark (`--benchmark`, first 5 valid frames excluded)
- bounded `FrameBuffer`
- `BackendPipeline` enqueue/drain skeleton
- frameCount, timestampUs, pointCount, 첫 point console 출력
- 선택적 A-B Adapter 및 SignalPipeline 호출/결과 진단 출력
- 명시적 Radar 재연결 옵션, 제한된 복구 및 연결별 B/시각/계측 상태 초기화
- WebSocket transport, 명시적 Mock JSON 송신 및 수동 요청 FIFO 수신

## 현재 구현되지 않음

- C State Machine / SafetyJudge 연결
- 실제 센서와 D 화면을 포함한 전체 통합 검증
- 수동 문 제어 실행 및 ACK
- Radar 처리의 producer/consumer multithreading
- 무응답 TCP 수신 idle timeout / heartbeat
- Device Discovery
- Target Section 상세 Parsing

Radar의 B/C 결과는 아래 시연 Adapter를 통해 WebSocket으로 발행합니다.
C State/Safety 및 실제 문 제어가 완료된 것으로 간주하지 않습니다.

기존 S0-3 TCP E2E와 WebSocket 반복 테스트는 각 브랜치의 이전 검증입니다.
통합 코드의 재검증 결과와 구분해야 하며 Winsock 10014의 근본 원인은 미확정입니다.

## 11/13 A-B-C-D 시연용 Radar Tracking 연결

원격 main `005a0c1`의 B/C 공개 타입과 기존 WebSocket 서버를 재사용합니다.
Radar 모드의 `--signal-config PATH --websocket` 조합에서만 활성화하며 `--benchmark`에서는
기존 측정 경로를 유지합니다. WebSocket 시작 실패 시 기존처럼 Radar/B 처리를 계속합니다.
`--ws-mock`, `--ws-door-sim` 및 위험 Mock 정책은 이 경로와 별개입니다.

매 프레임 `RadarFrame → toSignalFrame → SignalPipeline::process → toTrackingDetections →
TrackingEngine::processDetections`로 처리합니다. `processPoints()`나 두 번째 DBSCAN은 호출하지 않습니다.
Adapter의 `num_points >= 40`은 시연 전용입니다. 중심·bbox·z 범위·점 수·Doppler를 옮기고,
B에 없는 `mean_power=0`은 미측정 기본값입니다. 현재 C의 매칭/필터는 이 필드를 사용하지 않습니다.
빈 클러스터도 C에 전달합니다. C 객체는 TCP 세션 동안 유지하고 재연결 시 새로 생성합니다.

Snapshot은 v7의 기존 5개 필드를 사용합니다. `timestamp`는 발행 시점 UTC ISO 문자열,
`timestamp_us`는 B 결과 그대로, `door_state`는 B 추정값(UNKNOWN 포함), `door_command`는 null입니다.
`passengers`는 CONFIRMED 공개 Tracking ID와 x/y(m)의 전체 목록입니다. B cluster_id나 센서
Target ID를 사용하지 않습니다. `y < 0`은 OUTSIDE, 그 외 INSIDE인 시연용 분류이며
`safety_state=null`입니다. C State Machine/SafetyJudge 및 물리 문 완료 피드백이 아닙니다.
기본 C 설정은 3회 연속 매칭 후 CONFIRMED이며 미매칭 LOST는 화면 목록에서 제외됩니다.

모든 유효 B 결과를 C에 입력한 뒤 steady clock 기준 100ms가 지났을 때 현재 결과를 발행합니다.
입력 FPS와 발행 FPS는 다르며 저속 입력/스케줄링 지연에서는 10Hz보다 낮습니다.
입력이 없으면 오래된 결과를 반복 발행하지 않습니다. 기존 최신 Snapshot 교체/느린 클라이언트
정책을 유지합니다. 빈 목록도 발행하며 새 연결에 마지막 Snapshot을 자동 재전송하지 않습니다.
AB Profile의 B/AB 구간은 C 호출 전까지이고 pipeline/callback 구간에는 추가 처리가 포함됩니다.
수동 FIFO의 실행 정책은 변경하지 않으며 Radar 경로에서는 수신 명령을 문 제어로 실행하지 않습니다.

### 빌드 및 시험

C `tracking_core`의 Eigen3 CMake package가 필요합니다. `Eigen3_DIR`은 실제 `Eigen3Config.cmake`가
있는 디렉터리로 지정합니다. 이번 격리 빌드는 공식 Eigen 3.4.0 소스를 CMake로 설치한
`backend/build/abcd-demo-20261111/deps/eigen-install/share/eigen3/cmake`를 사용했습니다.
Boost 1.86.0 및 JSON 3.11.3은 기존 캐시를 재사용했습니다. C 소스는 수정하지 않고 Backend
CMake에서 MSVC `/utf-8`을 전달합니다. Backend 독립 구성에서도 tracking을 한 번만 추가합니다.

```powershell
cmake -S . -B backend/build/demo-root -A x64 -DEigen3_DIR=<Eigen3Config.cmake가-있는-경로>
cmake --build backend/build/demo-root --config Release
ctest --test-dir backend/build/demo-root -C Release --output-on-failure
cmake -S backend -B backend/build/demo-backend -A x64 -DEigen3_DIR=<동일-Eigen3-경로>
```

`tracking_adapter_test`는 필드/39·40 경계/빈 프레임/Tracking ID/CONFIRMED/UTC/문 상태/
100ms 제한/새 세션 초기화와 실제 B 호출을 검사합니다. Windows의 `tracking_radar_websocket_test`는
기존 wire layout의 합성 20Hz TCP 입력 64프레임을 실제 Backend로 보내 두 클라이언트 수신,
추적 ID·위치·빈 목록·입력 중단 시 발행 중단·EOF 종료를 검사합니다. 이는 FOV 실데이터 시험이 아닙니다.
기존 C 테스트도 등록합니다. `tracking/S0-1_t01.csv`가 없으면 `detection_loader_test`는 실패하며,
이를 제외한 결과를 전체 PASS로 표시하면 안 됩니다. 기존 Winsock 10014도 별도 실패로 보존합니다.

### FOV_t03 재생과 D 화면

Repository 루트에서, 배경맵과 빌드가 준비된 경우:

```powershell
.\backend\build\abcd-demo-20261111\root\backend\Release\elevsafe_backend.exe 127.0.0.1 29172 `
  --signal-config .\signal_processing\config\elevator_2nd.cfg `
  --websocket --ws-bind 127.0.0.1 --ws-port 8080
# 별도 터미널: D팀이 승인한 UNKNOWN 지원 소스에서
cd frontend
npm start
```

`signal_processing/data/background/elevator_2nd_closed.csv`는 Git 제외 데이터입니다. 이번 작업에서는 사용자가 제공한 바탕 화면의 원본을 복사했고 SHA256 일치를 확인했습니다.
시작 로그의 `background=enabled`를 확인합니다. 배경맵 로드 실패로 OFF가 된 결과를 정상적인
2차 엘리베이터 검증으로 취급하지 않습니다. 현재 고정 Mock 시각은 이 Radar 경로에서 사용하지 않습니다.

기존 Radar Emulator 실행 디렉터리에서 `radar-emulator.exe`를 실행하고 PCR 폴더를 열어
Default record를 FOV_t03으로 선택합니다. Stream은 29172, HTTP는 8000이며 Backend는 TCP에 직접 연결합니다.
Emulator는 최소화하지 않습니다. 첫 활성화는 임의 위치일 수 있으므로 `Restart from beginning`을
선택하고 첫 EOF 이후 Global 프레임이 1부터 시작하는 것을 확인합니다. 원본 단일 Session의
30~66초는 논리 인덱스 579~1281(0 기준), 703프레임입니다. UI의 1 기준 번호는 580~1282입니다.
직접 구간 seek/단발 종료는 지원하지 않습니다. FPS 설정에 따라 재생 시간은 달라지며,
Emulator의 stream frameCount와 Backend 수신 상대 시간은 PCR 원본 번호/delta 시간과 같지 않습니다.

D 접속 주소는 `ws://127.0.0.1:8080/radar`입니다. PR #34에는 UNKNOWN 수신 및 '확인 중' 표시가
있지만 이번 기준 main에는 없습니다. D 제품 소스는 수정하지 않습니다. UNKNOWN 화면 검증에는
D팀의 해당 변경이 반영된 실행 환경이 필요합니다. Backend 수신 성공과 D GUI 성공은 구분합니다.
실제 FALL/IMMOBILE, 위험 자동 OPEN/HOLD, HALL 상태 분류 및 엘리베이터 제어는 구현하지 않았습니다.

### 이번 통합 검증 결과 (2026-11-11)

| 검증 | 결과 |
|---|---|
| Windows MSVC x64 Release 루트 / Backend 독립 Build | 모두 PASS |
| 루트 전체 CTest | 22/24 PASS, ExitCode 8 |
| Backend 독립 전체 CTest | 23/24 PASS, ExitCode 8 |
| 신규 Adapter / 합성 TCP-WebSocket 통합 시험 | 양쪽 PASS |

루트의 `websocket_server_test`는 null-compatibility의 server.start/listen에서
요청 포트 0, system:10014로 실패했습니다. 원인은 미확정이며 재시도로 대체하지 않았습니다.
양쪽 `detection_loader_test`는 기존 C 입력 `tracking/S0-1_t01.csv` 부재로 실패했습니다.
C의 id_manager/main 시험 및 기존 A–B·Background ON/OFF·Radar 재연결 시험은 통과했습니다.
처음 루트 빌드에서 C 테스트의 MSVC 문자 인코딩 오류가 발생했고, Backend CMake의 `/utf-8`
적용 후 빌드는 통과했습니다. 최초 실패 로그도 보존했습니다.

합성 TCP 시험은 각 구성에서 입력 64프레임을 모두 처리하고 두 클라이언트가 동일한
25개 Snapshot을 받았습니다. Snapshot의 평균 source timestamp 간격은 루트 124.082ms,
독립 128.844ms였습니다. 고정 10Hz 또는 실제 센서 입력 성능을 검증한 수치가 아닙니다.
Tracking ID/좌표, 빈 목록, 입력 공백에서 발행 중단 및 Backend EOF ExitCode 0을 확인했습니다.
수신 공백 검사를 위한 클라이언트 취소 시 연결이 abort되므로 정상 close handshake 검증과는 구분합니다.

FOV_t03 원본 및 배경맵 파일은 확인했지만, GUI 도구의 native pipe 연결 실패로 Emulator의
파일 선택·재생은 완료하지 못했습니다. 실행 시도한 Emulator는 종료했습니다.
따라서 실제 FOV Background ON 재생 및 D React 화면 표시는 미검증입니다.
로그는 Git 제외 `backend/build/abcd-demo-20261111/`에 보존합니다.
