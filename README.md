# ElevSafe

4D 레이더(Retina-4SN)로 엘리베이터 출입구와 내부의 승객을 감지하고, 낙상 등 이상 상태를 관제하는 시스템입니다.

## 모듈 구성

| 폴더 | 담당 | 역할 |
|---|---|---|
| [`backend/`](backend/) | A (김진) | 센서 수신·파싱, 프레임 버퍼, 메인 파이프라인, WebSocket |
| [`signal_processing/`](signal_processing/) | B (김수연) | 좌표 변환, ROI, 정적 배경 제거, DBSCAN 클러스터링 |
| [`tracking/`](tracking/) | C (김현진) | 칼만 필터 추적, ID 관리, 상태 머신·낙상 판단 |
| [`frontend/`](frontend/) | D (서채민) | React 관제 대시보드 |

```
센서 → A (수신·파싱) → B (신호 처리) → C (추적·상태) → A (JSON 전송) → D (화면)
```

각 모듈의 빌드·실행 방법과 세부 설계는 폴더별 README를 참고해 주세요.

## Windows 루트 빌드 및 테스트

Visual Studio 2022의 C++ 개발 도구와 CMake가 설치된 환경에서 Repository 루트에서 실행합니다.
루트는 Backend를 등록하고, Backend가 B의 `radar_core`를 한 번 포함합니다.

```powershell
cmake -S . -B build-integrated -G "Visual Studio 17 2022" -A x64
cmake --build build-integrated --config Release
ctest --test-dir build-integrated -C Release -N
ctest --test-dir build-integrated -C Release --output-on-failure
```

Backend 실행 파일과 테스트 도구는 `build-integrated/backend/Release/`에 생성됩니다.
설정·데이터의 상대 경로를 유지하려면 실행할 때도 Repository 루트를 작업 디렉터리로 사용합니다.
WebSocket 의존성은 기존 Backend 설정의 Boost 1.86.0과 nlohmann/json 3.11.3을 사용하며,
새 빌드 디렉터리에서는 최초 구성 시 다운로드될 수 있습니다.
이미 확보한 소스를 재사용하려면 구성 명령에
`-DFETCHCONTENT_SOURCE_DIR_ELEVSAFE_BOOST="<Boost 소스 디렉터리>"`와
`-DFETCHCONTENT_SOURCE_DIR_ELEVSAFE_JSON="<JSON 소스 디렉터리>"`를 추가합니다.

기존 Backend 독립 빌드도 유지됩니다.

```powershell
cmake -S backend -B backend/build-websocket-integration -G "Visual Studio 17 2022" -A x64
cmake --build backend/build-websocket-integration --config Release
ctest --test-dir backend/build-websocket-integration -C Release --output-on-failure
```

B의 `radar_pipeline`·`build_background` 도구는 [B 독립 빌드](signal_processing/README.md)로 생성합니다.
D Frontend는 [기존 npm 빌드](frontend/README.md)를 사용하며, C Tracking과 D는 루트 CMake 대상에 포함하지 않습니다.
테스트에서 발생하는 기존 Winsock 10014는 별도 조사 중인 문제이며 테스트를 제외하지 않습니다.

## 공통 좌표계

- 원점: 캐빈 문턱 중앙의 바닥
- x: 승강장에서 캐빈을 바라볼 때 오른쪽 +
- y: 캐빈 안쪽 +, 승강장 쪽 −
- z: 바닥 0, 위쪽 +
- 단위: m (오른손 좌표계)

## 진행 상황

주차별 태스크와 진행 상황, 회의 결정 사항은 **노션에서 관리**합니다.
(여러 명이 같은 체크리스트 줄을 동시에 고쳐 충돌이 나는 것을 막기 위해, README에는 체크리스트를 두지 않습니다.)

- [마일스톤 및 주차별 Task](https://app.notion.com/p/3e5718d4853e80f9a4b4fa77e0991454)
- [회의록](노션 회의록 페이지 링크)
