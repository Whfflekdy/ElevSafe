# ElevSafe Backend

ElevSafe Backend 1차 구현은 Retina-4SN 또는 Radar Emulator의 TCP point-cloud stream을 수신하고, packet과 frame을 파싱하는 단계입니다.

현재 데이터 흐름에서는 Radar Interface와 Parsing 단계까지 구현되어 있습니다.

```text
Retina-4SN
    -> Backend TCP/Parser
    -> Signal Processing
    -> Tracking/State
    -> Backend Result Aggregation
    -> WebSocket
    -> Frontend
```

## 현재 구현

- Retina-4SN / Radar Emulator TCP client
- TCP `29172` direct connection
- Raw binary stream 수신
- stream buffer 기반 packet boundary 복원
- partial `recv` 처리
- 여러 packet 연속 처리
- packet magic 및 `packageSize` 검증
- Radar Frame parsing
- Point Cloud parsing
- `RadarPoint`: `x`, `y`, `z`, `doppler`, `power`, `targetId`
- `RadarFrame`: `frameCount`, point vector
- console 출력
  - `frameCount`
  - `pointCount`
  - 첫 point의 `x`, `y`, `z`, `doppler`, `power`, `targetId`

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

현재 parser 테스트는 partial packet, 여러 packet, garbage resync, 잘못된 magic, 비정상 `packageSize`, point count 경계와 trailing payload를 검증합니다.

## 실행

### Radar Emulator

Radar Emulator를 실행하고 `.pcr` 데이터셋을 연 뒤 재생을 시작합니다.

```powershell
.\backend\build\Release\elevsafe_backend.exe 127.0.0.1 29172
```

### Retina-4SN

장치 IP를 직접 지정합니다. Device Discovery는 아직 구현하지 않았습니다.

```powershell
.\backend\build\Release\elevsafe_backend.exe <RADAR_IP> 29172
```

실제 Retina-4SN 장치에 대한 연결과 packet 수신은 아직 검증하지 않았습니다.

## 검증 결과

- CMake Release build 성공
- CTest 100% 통과
- Radar Emulator TCP `29172` 연결 성공
- Emulator Clients `0 -> 1` 확인
- `.pcr` 데이터에서 `frameCount` 연속 증가 확인
- `pointCount` 정상 출력
- `x/y/z/doppler/power/targetId` 정상 파싱 확인

## 미구현 또는 미확인

- Device Discovery
- 실제 Retina-4SN packet 검증
- target section parsing
- reference code의 `48056` offset 의미
- firmware별 packet 차이
- automatic reconnect
- multithreaded processing pipeline
- signal processing / DBSCAN
- tracking / state machine
- WebSocket / frontend output
