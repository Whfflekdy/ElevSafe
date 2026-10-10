# ElevSafe Backend

ElevSafe Backend는 Retina-4SN 또는 Radar Emulator에서 TCP raw binary
stream을 수신하고, packet/frame 경계를 복원하여 `RadarFrame`으로 변환하는
Radar interface와 초기 처리 파이프라인을 담당합니다.

현재 Backend는 수신한 frame을 `BackendPipeline`과 `FrameBuffer`를 거쳐
console consumer로 전달하는 단일 스레드 runtime까지 구현되어 있습니다.
B Signal Processing, C Tracking/State, D Frontend는 현재 Backend에 연결되어
있지 않습니다.

## 디렉터리 구조

```text
backend/
├─ CMakeLists.txt
├─ README.md
├─ include/
│  └─ elevsafe/
│     ├─ radar_types.h
│     ├─ relative_frame_clock.h
│     ├─ retina_protocol.h
│     ├─ retina_client.h
│     ├─ frame_buffer.h
│     └─ backend_pipeline.h
├─ src/
│  ├─ main.cpp
│  ├─ retina_protocol.cpp
│  ├─ retina_client.cpp
│  ├─ frame_buffer.cpp
│  └─ backend_pipeline.cpp
└─ tests/
   ├─ retina_protocol_test.cpp
   ├─ frame_buffer_test.cpp
   ├─ backend_pipeline_test.cpp
   └─ relative_frame_clock_test.cpp
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
- B Signal Processing과 C Tracking/State 실제 모듈은 연결되어 있지 않습니다.

### main.cpp

`main.cpp`는 host와 port를 읽고 `RetinaClient`를 실행합니다. callback에서
`onFrame()`을 호출한 직후 `drain()`을 호출하므로 현재 runtime은 별도
worker thread 없는 single-thread synchronous 구조입니다. 기존의
`frameCount`, `timestampUs`, `pointCount`, 첫 point의 `x/y/z/doppler/power/targetId`
출력은 `drain()`에 전달된 console consumer가 담당합니다.

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

## Build

프로젝트 루트에서 실행합니다.

```powershell
cmake -S backend -B backend/build
cmake --build backend/build --config Release
```

## Test

```powershell
ctest --test-dir backend/build -C Release --output-on-failure
```

현재 CMake에 등록된 테스트는 다음 4개입니다.

- `retina_protocol_test`: packet/frame parsing 및 stream edge case
- `frame_buffer_test`: bounded FIFO, FIFO 순서, oldest frame 제거 및 capacity 검증
- `backend_pipeline_test`: frame enqueue, FIFO drain, consumer 동작 및 overflow 전달 검증
- `relative_frame_clock_test`: connection-relative timestamp 계산 및 reset 검증

Release 기준 현재 검증 결과는 CTest `4/4` 통과입니다.

## 검증 상태

Radar Emulator에 대해서는 다음을 확인했습니다.

- TCP `29172` 연결 성공
- Emulator Clients `0 -> 1` 확인
- `.pcr` 데이터에서 `frameCount` 연속 증가 확인
- `pointCount` 정상 출력
- `x/y/z/doppler/power/targetId` 정상 파싱 확인

실제 RETINA-4SN에 대해서는 AP Mode 및 TCP `29172` 연결 후
frame 연속 수신과 parsing을 확인했습니다.

## 실행

### Radar Emulator

Radar Emulator를 실행하고 `.pcr` 데이터셋을 연 뒤 재생을 시작합니다.

```powershell
.\backend\build\Release\elevsafe_backend.exe 127.0.0.1 29172
```

### RETINA-4SN

Device Discovery 없이 장치 IP를 직접 지정합니다.

```powershell
.\backend\build\Release\elevsafe_backend.exe 192.168.30.1 29172
```

실제 RETINA-4SN에서는 AP Mode 및 TCP `29172` 연결 후
`frameCount`, `pointCount`, `x/y/z/doppler/power/targetId`의 연속
수신 및 parsing까지 검증했습니다. 실제 연결 가능 여부와 데이터 수신은
PC IP 설정 및 Radar의 Client 접근 허용 IP 설정에 영향을 받을 수 있습니다.

host와 port는 command line으로 지정할 수 있습니다. port는 `1`부터
`65535`까지 허용됩니다. host를 생략하면 `main.cpp`에서
`127.0.0.1`을 사용하고, port를 생략하면 `29172`를 사용합니다.

### Parser Latency Benchmark

Radar Emulator로 PCR을 한 번 재생하며 A 수신/파싱 구간을 측정하려면 다음처럼
`--benchmark`를 마지막 인자로 지정합니다.

```powershell
.\backend\build\Release\elevsafe_backend.exe 127.0.0.1 29172 --benchmark
```

Radar Emulator의 loop playback에서 첫 N개의 valid frame만 측정하려면
`--benchmark-frames N`을 함께 지정합니다. 예를 들어 164-frame PCR의 공식 측정은
다음과 같습니다.

```powershell
.\backend\build\Release\elevsafe_backend.exe 127.0.0.1 29172 --benchmark --benchmark-frames 164
```

이 모드는 기존 frame별 console 출력을 생략하지만, 동일한 synchronous
`BackendPipeline` enqueue/drain 경로는 유지합니다. `RetinaStreamParser`가 valid
`RadarFrame`을 완성하기 전의 parser core latency만 기록하므로 `recv()` 대기,
network/replay interval, `timestampUs` 설정, pipeline 및 console I/O는 측정값에
포함되지 않습니다. 처음 5개의 valid frame은 warm-up으로 제외하며 이후 frame의
average, nearest-rank p95, maximum을 milliseconds 단위로 출력합니다. frame limit에
도달하면 summary를 한 번 출력하고 이후 반복 frame은 parsing을 계속하되 benchmark
통계에는 추가하지 않습니다.

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

## 현재 구현되지 않음

- B Signal Processing 실제 연결
- C Tracking / State 실제 연결
- WebSocket Server
- JSON Output
- Multithreading
- Automatic Reconnect
- Device Discovery
- Target Section 상세 Parsing

향후 다른 모듈이 연결될 수 있지만, 현재 Backend runtime이 생성하거나
전송하는 결과는 console 출력까지입니다. 위 미구현 영역을 현재 완료된
기능으로 간주하지 않습니다.
