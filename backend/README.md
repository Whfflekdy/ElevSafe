# ElevSafe Backend

ElevSafe Backend는 Retina-4SN 또는 Radar Emulator에서 TCP raw binary
stream을 수신하고, packet/frame 경계를 복원하여 `RadarFrame`으로 변환하는
Radar interface와 초기 처리 파이프라인을 담당합니다.

현재 Backend는 수신한 frame을 `BackendPipeline`과 `FrameBuffer`를 거쳐
consumer로 전달하는 단일 스레드 runtime입니다. `--signal-config PATH`를
지정하면 Adapter를 통해 B `SignalPipeline`을 호출하고 결과를 console에 출력합니다.
옵션을 생략하면 기존 수신/console 경로를 유지합니다. C Tracking/State와
D Frontend는 현재 Backend에 연결되어 있지 않습니다.

기존 WebSocket transport와 명시적 Mock 모드가 A-B 실행 경로와 공존합니다.
WebSocket 전용 코드는 `7c86f77`의 기존 구현을 재사용했으며, 실제 B/C 결과를
승객 Snapshot으로 전송하거나 수동 문 제어를 실행하지 않습니다.

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
- C Tracking/State 실제 모듈은 연결되어 있지 않습니다.

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

현재 CMake에 등록된 테스트는 다음 12개입니다.

- `retina_protocol_test`: packet/frame parsing 및 stream edge case
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
- `websocket_server_test`: loopback handshake, 경로, 다중 접속, 요청 FIFO,
  느린 client, 큐 교체, 큰 메시지, 연결 해제/재접속, pending I/O 종료
- `websocket_cli_test`: 기존 CLI 오류, Mock 실패 시 Radar 미진입, WebSocket 시작 실패 후
  Radar 계속 실행을 검증합니다. Windows 기본 PowerShell/.NET fixture가 임시 loopback 포트로
  두 프레임을 보내 Radar-only 및 WebSocket 실패 모드의 B 처리·정상 EOF를 확인합니다.
  기존 Parser 테스트의 패킷 형식을 사용하며 외부 29172 서버나 새 런타임 설치에 의존하지 않습니다.
  fixture 소켓 오류는 실제 코드와 함께 실패 처리하며 재시도하지 않습니다. 개별 자식 프로세스는
  제한 시간 내에 끝나야 하며 fixture 실패 로그는 실행 파일 옆 `cli-radar-fixture/`에 보존합니다.

테스트는 로컬의 실제 배경 맵에 의존하지 않습니다. Background 테스트만 빌드 폴더의
새 임시 디렉터리에 작은 맵과 설정 사본을 만들며 실제 센서 설치 설정/맵은 변경하지 않습니다.

서버 테스트는 8080/8081/8082 port를 사용하고 timeout 및 직렬 실행을 설정합니다.
테스트 전에 해당 port의 기존 서버를 정상 종료하세요. 로컬 반복 검증에서는 listen/connect
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
실제 C 결과의 10Hz 갱신이나 새 결과가 없을 때의 재전송 정책은 아직 구현하지 않았습니다.

`--mock-websocket`은 기존 `--ws-mock`의 별칭입니다. Mock은 host/port,
benchmark 및 `--signal-config`와 함께 사용할 수 없습니다.

### 레이더 수신과 WebSocket transport 동시 실행

```powershell
.\backend\build-websocket-integration\Release\elevsafe_backend.exe 127.0.0.1 29172 --websocket
```

이 모드는 기존 TCP/Parser/FrameBuffer/console 경로와 WebSocket 접속·요청 수신을
함께 실행합니다. C 결과가 없으므로 **실시간 Snapshot은 생성하지 않습니다.**
옵션을 생략하면 서버를 시작하지 않고 기존 동작을 유지합니다. Radar 모드에서 WebSocket
시작이 실패하면 원래 예외 정보를 `[ws] startup failed:`로 출력하고 실패 객체를 정리합니다.
`[ws] disabled for this run; continuing radar processing`을 기록한 뒤 활성 WebSocket 없이
Radar TCP 수신과 선택적 B 처리를 계속합니다. 재시도는 하지 않으며 최종 종료 코드는
기존 Radar 실행 결과를 따릅니다. TCP 실패/정상 EOF로 run이 반환하면 서버도 정리합니다.
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

### JSON 및 수동 요청

v6 송신은 `timestamp`, `timestamp_us`, `door_command`, `door_state`, `passengers`를
포함한 text JSON입니다. `timestamp_us`는 세션 첫 프레임 기준 경과 µs의 음수가 아닌 정수입니다.
누락, 음수, 실수, 문자열, null/bool은 거부하며 별도 `alert` 필드는 허용하지 않습니다.
`timestamp`는 유지하며 상대 µs를 절대시각이나 처리 지연으로 해석하지 않습니다.
승객은 `id`, 유한한 숫자 `x/y`(m), `state`, `safety_state`, 선택적 `label`을 사용합니다.
`state`는 OUTSIDE/ENTERING/INSIDE/EXITING, `safety_state`는 null/FALL/IMMOBILE입니다.
door_state는 OPEN/CLOSED/OPENING/CLOSING, door_command는 null/OPEN/HOLD/CLOSE입니다.
UNKNOWN 변환이나 z 필드 추가는 하지 않습니다. ID는 정수 또는 문자열을 허용하며
최종 Tracking ID 타입과 실제 결과 공급은 C 통합 시 확인해야 합니다.

현재 수동 요청은 다음처럼 **door_command 한 필드만** 받습니다.

```json
{"door_command":"OPEN"}
```

OPEN/HOLD/CLOSE는 로그와 `popCommand()` FIFO에 전달합니다. JSON 오류, 비객체,
필드 누락, null/알 수 없는 명령, 추가 필드, binary 메시지는 거부합니다.
**수신은 실행 완료가 아닙니다.** ACK, 문 상태 변경, MANUAL/AUTO 우선순위 적용은 없습니다.
Mock에서는 수신 큐를 비우지만 요청에 따라 Mock 상태를 바꾸지 않습니다.
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

D 코드 변경은 이번 작업에 포함하지 않습니다. D 담당자가 `frontend/src/App.jsx`의
`USE_REAL_WEBSOCKET=true`, `RADAR_WS_URL='ws://localhost:8080/radar'`를 선택한 뒤
기존 `npm start`로 실행하면 Mock과 연결할 수 있습니다. 이 절차의 실행 여부는 별도 보고합니다.
빈 passengers는 전체 목록 제거, 필드 생략은 목록 유지라는 D의 현재 동작을 고려해야 합니다.

기본 bind는 loopback이지만 인증·Origin 제한·TLS는 구현하지 않았습니다. 외부 bind는
신뢰된 테스트 환경에서만 사용하고 실제 문 제어 시스템에 연결하지 마세요. HTTPS UI에는
wss가 필요하지만 이 서버의 TLS 구성은 현재 범위가 아닙니다.

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
- WebSocket transport, 명시적 Mock JSON 송신 및 수동 요청 FIFO 수신

## 현재 구현되지 않음

- C Tracking / State 실제 연결
- 실제 B/C 결과의 WebSocket Snapshot 생성 및 전송
- 수동 문 제어 실행 및 ACK
- Radar 처리의 producer/consumer multithreading
- Automatic Reconnect
- Device Discovery
- Target Section 상세 Parsing

실제 레이더/B 처리 결과는 console까지이며 WebSocket JSON 송신은 명시적 Mock에
한정됩니다. C/D 통합 및 실제 문 제어가 완료된 것으로 간주하지 않습니다.

기존 S0-3 TCP E2E와 WebSocket 반복 테스트는 각 브랜치의 이전 검증입니다.
통합 코드의 재검증 결과와 구분해야 하며 Winsock 10014의 근본 원인은 미확정입니다.
