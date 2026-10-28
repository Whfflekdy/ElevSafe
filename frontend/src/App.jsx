import React, { useState, useEffect, useRef } from 'react';
import './App.css';

// 🛠️ [실제 도면 규격 설정 - 단위: cm]
// 이제 px 단위로 머리 아프게 고민할 필요 없이 실제 줄자로 잰 cm 길이를 넣으세요!
const CONFIG_CM = {
  ELEV_WIDTH: 162,      // 카 폭 (가로)
  ELEV_DEPTH: 137,      // 카 깊이 (세로)
  DOOR_WIDTH: 88,       // 출입구 폭
  DOOR_THICKNESS:24,    // 문틈 두께 (세로 폭)
  WAITING_DEPTH: 90,   // 승강장(대기 구역) 길이
  WAITING_WIDTH: 166,   // 승강장 최대 폭
  TRAP_DEPTH: 52,       // 사다리꼴 깊이
  TRAP_BOTTOM_W: 101,   // 사다리꼴 하단 폭
};

// 🔍 화면 확대/축소 비율 (1cm = 몇 px로 그릴 것인가?)
// 도면이 화면에 비해 너무 작으면 이 숫자를 2.5, 3.0으로 늘리고, 너무 크면 1.5 등으로 줄이세요.
const SCALE = 1.6;


/* [실제 연동 시 바꿀 설정]
 * false: 예시 승객과 문 애니메이션으로 UI 테스트 (서버 연결 안 함)
 * true : WebSocket으로 데이터 수신 및 명령 전송
 * HTTPS 화면에서는 서버도 wss:// 주소를 사용해야 합니다.
 */
const USE_REAL_WEBSOCKET = false;
const RADAR_WS_URL = 'ws://localhost:8080/radar';
const MAX_WIDTH = Math.max(CONFIG_CM.ELEV_WIDTH, CONFIG_CM.WAITING_WIDTH);
// 외곽선을 한 번 계산합니다. 내부 구분선과 면 채우기는 없습니다.
const PLAN = (() => {
  const c = MAX_WIDTH / 2;
  const cabinL = c - CONFIG_CM.ELEV_WIDTH / 2;
  const cabinR = c + CONFIG_CM.ELEV_WIDTH / 2;
  const doorL = c - CONFIG_CM.DOOR_WIDTH / 2;
  const doorR = c + CONFIG_CM.DOOR_WIDTH / 2;
  const trapL = c - CONFIG_CM.TRAP_BOTTOM_W / 2;
  const trapR = c + CONFIG_CM.TRAP_BOTTOM_W / 2;
  const waitL = c - CONFIG_CM.WAITING_WIDTH / 2;
  const waitR = c + CONFIG_CM.WAITING_WIDTH / 2;
  const cabinBottom = CONFIG_CM.ELEV_DEPTH;
  const doorBottom = cabinBottom + CONFIG_CM.DOOR_THICKNESS;
  const waitingTop = doorBottom + CONFIG_CM.TRAP_DEPTH;
  const height = waitingTop + CONFIG_CM.WAITING_DEPTH;

  return {
    height,
    waitingTop,
    path: `M ${cabinL} 0 H ${cabinR} V ${cabinBottom}
      H ${doorR} V ${doorBottom} L ${trapR} ${waitingTop}
      H ${waitR} V ${height} H ${waitL} V ${waitingTop}
      H ${trapL} L ${doorL} ${doorBottom} V ${cabinBottom}
      H ${cabinL} Z`,
  };
})();

// 도면, 구역 이름, 문 애니메이션을 담당합니다. children에는 승객 점이 들어갑니다.
function FloorPlan({ doorState, children }) {
  return (
    <div
      className="floor-plan"
      style={{ width: MAX_WIDTH * SCALE, height: PLAN.height * SCALE }}
    >
      {/* 기존 하늘색 외곽선 */}
      <svg
        className="floor-outline"
        viewBox={`0 0 ${MAX_WIDTH} ${PLAN.height}`}
        role="img"
        aria-label="캐빈과 출입구, 사다리꼴 연결부, 승강장 평면도"
      >
        <path d={PLAN.path} />
      </svg>

      {/* 공간 이름은 승객 클릭을 방해하지 않도록 CSS에서 pointer-events를 끕니다. */}
      <span className="plan-label" style={{ top: CONFIG_CM.ELEV_DEPTH * SCALE / 2 }}>
        캐빈 내부 (Cabin)
      </span>
      <span
        className="plan-label"
        style={{ top: (PLAN.waitingTop + CONFIG_CM.WAITING_DEPTH / 2) * SCALE }}
      >
        승강장 (Waiting Area)
      </span>

      {/* 실제 문 영역: 문 상태에 따라 양쪽 문짝을 수축/복원합니다. */}
      <div
        className={`plan-door-frame ${doorState === 'OPEN' || doorState === 'OPENING' ? 'is-open' : ''}`}
        style={{
          left: (MAX_WIDTH - CONFIG_CM.DOOR_WIDTH) * SCALE / 2,
          top: CONFIG_CM.ELEV_DEPTH * SCALE,
          width: CONFIG_CM.DOOR_WIDTH * SCALE,
          height: CONFIG_CM.DOOR_THICKNESS * SCALE,
        }}
      >
        <div className="plan-door plan-door-left" />
        <div className="plan-door plan-door-right" />
      </div>
      {children}
    </div>
  );
}


function App() {
  const [doorState, setDoorState] = useState('CLOSED');
  const [doorCommand, setDoorCommand] = useState(null);
  const [selectedDot, setSelectedDot] = useState(null); // 클릭한 승객 말풍선 띄우기용
  // 실제 연동에서는 빈 배열로 시작하고 서버 데이터로 채웁니다.
  // 테스트용 x, y도 m 단위이며, 문 중앙 기준 좌표입니다.
  const [passengers, setPassengers] = useState(USE_REAL_WEBSOCKET ? [] : [
    { id: 1, x: 0, y: 1, state: 'INSIDE', safety_state: null, label: 'Passenger #1' },
    { id: 2, x: 0.25, y: 1, state: 'INSIDE', safety_state: null, label: 'Passenger #2' },  
    { id: 3, x: 0, y: 0, state: 'ENTERING', safety_state: null, label: 'Passenger #3' },
    { id: 4, x: -0.25, y: 1, state: 'OUTSIDE', safety_state: null, label: 'Passenger #4' }
  ]);

  const [systemLogs, setSystemLogs] = useState([
    USE_REAL_WEBSOCKET
      ? '[WebSocket] 서버 연결 대기 중...'
      : '[테스트 모드] 실제 데이터 연동 전 UI 테스트 상태입니다.'
  ]);
  const wsRef = useRef(null);
  const doorTimerRef = useRef(null);

  /* 
  ===============================================================
  [실제 넣어야 할 부분 - WebSocket 연동 영역]

  아래 코드는 미리 구현되어 있습니다. 주석을 해제할 필요 없이
  파일 상단의 USE_REAL_WEBSOCKET을 true로 바꾸고
  RADAR_WS_URL을 실제 서버 주소로 바꾸면 활성화됩니다.

  수신 JSON 예시:
  {
    "timestamp": "2026-10-01T14:02:05Z",
    "door_state": "OPEN",
    "door_command": null,
    "passengers": [
      { "id": 1, "x": 0.25, "y": 1.0, "state": "INSIDE",
        "safety_state": null, "label": "Passenger #1" }
    ]
  }

  door_state: OPEN / CLOSED / OPENING / CLOSING
  door_command: null / OPEN / HOLD / CLOSE
  state: INSIDE / OUTSIDE / ENTERING / EXITING
  safety_state: null / FALL / IMMOBILE
  x, y: 유한한 숫자, 단위 m, 문 중앙이 (0, 0)
  화면에서 x 양수는 오른쪽, y 양수는 캐빈 안쪽(위쪽)입니다.

  이 예제에는 자동 재연결, 인증 및 명령 응답 확인은 포함하지 않았습니다.
  서버에서 passengers를 보내면 화면의 전체 승객 목록을 교체합니다.
  =================================================================
  */

  useEffect(() => {
    if (!USE_REAL_WEBSOCKET) return;

    const ws = new WebSocket(RADAR_WS_URL);
    wsRef.current = ws;
    const log = message => setSystemLogs(prev => [...prev, message]);

    ws.onopen = () => log('[WebSocket] 서버 연결 완료');
    ws.onmessage = event => {
      try {
        const data = JSON.parse(event.data);
        if (!data || typeof data !== 'object' || Array.isArray(data)) {
          throw new Error('JSON 객체가 필요합니다.');
        }
        if (['OPEN', 'CLOSED', 'OPENING', 'CLOSING'].includes(data.door_state)) {
          setDoorState(data.door_state);
        }
        if (Array.isArray(data.passengers)) {
          // 좌표가 없거나 숫자가 아닌 항목은 도면에 표시하지 않습니다.
          setPassengers(data.passengers.filter(p =>
            p && (typeof p.id === 'number' || typeof p.id === 'string') &&
            Number.isFinite(p.x) && Number.isFinite(p.y)
          ));
        }
        if ([null, 'OPEN', 'HOLD', 'CLOSE'].includes(data.door_command)) {
          setDoorCommand(data.door_command);
        }
      } catch (error) {
        console.error('WebSocket 수신 데이터 처리 오류:', error);
        log('[WebSocket] 수신 데이터 형식 오류');
      }
    };
    ws.onerror = () => log('[WebSocket] 연결 오류 발생');
    ws.onclose = () => log('[WebSocket] 연결 종료');

    return () => {
      // React 개발 모드 재실행 및 화면 종료 시 기존 연결을 정리합니다.
      ws.onopen = ws.onmessage = ws.onerror = ws.onclose = null;
      if (wsRef.current === ws) wsRef.current = null;
      ws.close();
    };
  }, []);

  // 테스트 도중 화면이 닫히면 남아 있는 타이머도 정리합니다.
  useEffect(() => () => clearTimeout(doorTimerRef.current), []);

  // 승객 점의 색상은 기존 안전 상태 기준을 유지합니다.
  const getTheme = (safetyState) => {
    if (safetyState === 'FALL' || safetyState === 'IMMOBILE') return 'danger';
    return 'normal';
  };
  // 기존 동작 유지: OUTSIDE가 아닌 승객의 IMMOBILE만 전체 경보를 활성화합니다.
  // FALL도 전체 경보에 포함하려면 이 조건을 별도로 변경하세요.
  const isDangerActive = passengers.some(
    p => p.safety_state === 'IMMOBILE' && p.state !== 'OUTSIDE'
  );

useEffect(() => {
  if (isDangerActive) {
    const audio = new Audio('/alert-sound.mp3'); 
    audio.play().catch(e => console.log("브라우저 자동재생 정책 제한"));
  }
}, [isDangerActive]);
  const getStatusLabel = (state, safetyState) => {
    if (safetyState === 'FALL') return '🚨 비상 (낙상 감지!)';
    if (safetyState === 'IMMOBILE') return '🚨 비상 (장기 미동 감지!)';
    
    switch (state) {
      case 'OUTSIDE': return '🧍 대기 중 (승강장)';
      case 'ENTERING': return '🚶 입장 중 (정상)';
      case 'INSIDE': return '✅ 정상 탑승';
      case 'EXITING': return '🚶 퇴장 중 (정상)';
      default: return '알 수 없음';
    }
  };
  // 실제 모드에서는 명령만 전송하고, 문의 실제 상태는 서버 수신값으로 표시합니다.
  const handleDoorCommand = command => {
    const timeStr = new Date().toLocaleTimeString();

    if (USE_REAL_WEBSOCKET) {
      const ws = wsRef.current;
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        setSystemLogs(prev => [...prev, `[${timeStr}] 연결되지 않아 명령을 전송하지 못했습니다.`]);
        return;
      }
      try {
        // [실제 명령 전송 영역] 백엔드와 이 JSON 형식을 맞추세요.
        ws.send(JSON.stringify({ door_command: command }));
        setDoorCommand(command);
        setSystemLogs(prev => [...prev, `[${timeStr}] 📤명령 송신: ${command} (서버 처리 확인 전)`]);
      } catch (error) {
        console.error('명령 전송 오류:', error);
        setSystemLogs(prev => [...prev, `[${timeStr}] 명령 전송 실패`]);
      }
      return;
    }

    // [테스트 전용] 실제 모드에서는 위에서 return하므로 실행되지 않습니다.
    // 새 명령이 오면 이전 타이머를 취소하여 상태가 뒤늦게 덮이는 것을 방지합니다.
    clearTimeout(doorTimerRef.current);
    setDoorCommand(command);
    setSystemLogs(prev => [...prev, `[${timeStr}] 테스트 명령: ${command}`]);
    if (command === 'HOLD') {
      setDoorState('OPEN');
    } else {
      const opening = command === 'OPEN';
      setDoorState(opening ? 'OPENING' : 'CLOSING');
      doorTimerRef.current = setTimeout(() => {
        setDoorState(opening ? 'OPEN' : 'CLOSED');
      }, 1200);
    }
  };
  const handleBackgroundClick = () => {
    setSelectedDot(null);
  };

  const selectedPassenger = passengers.find(p => p.id === selectedDot);

  return (
    <div className={`dashboard-container ${isDangerActive ? 'danger-background' : ''}`} onClick={handleBackgroundClick}>
      <header className="header">
        <h1>ElevSafe 관제 시스템 — [RETINA-4SN 4D Radar 연동]</h1>
        <div className="spec-badge">
          <span>Range: 12m(Person) / 7m×7m</span> | <span>FoV: 90°(±45°)</span> | <span>Rate: 50ms</span>
        </div>
      </header>

      <div className="main-content">

        <div className="left-panel" onClick={(e) => e.stopPropagation()}>
          <h2>승객 상세 정보</h2>
          {selectedPassenger ? (
            <div className={`info-box ${getTheme(selectedPassenger.safety_state) === 'danger' ? 'alert' : ''}`}>
              <h3>ID: {selectedPassenger.id}</h3>
              <strong>{selectedPassenger.label}</strong>
              <p>수신 좌표:<br/>X {selectedPassenger.x}m / Y {selectedPassenger.y}m</p>
              <p className="state-text">
                상태: {getStatusLabel(selectedPassenger.state, selectedPassenger.safety_state)}
              </p>
              <button onClick={() => setSelectedDot(null)}>선택 해제</button>
            </div>
          ) : (
            <div className="empty-info">
              <p>도면에서 승객(점)을 클릭하면<br/>여기에 상세 정보가 표시됩니다.</p>
            </div>
          )}
        </div>

        <div className="visualization-section">
          <h2>엘리베이터 및 승강장 2D 평면도 (1cm = {SCALE}px Scale)</h2>
          
          <div className="canvas-container">

            <FloorPlan doorState={doorState}>

              {passengers.map(p => {
                const theme = getTheme(p.safety_state);
                // 수신 좌표 m를 cm로 바꾼 뒤 화면 배율을 적용합니다.
                const x_cm = p.x * 100;
                const y_cm = p.y * 100;
                const leftPos = (MAX_WIDTH / 2 + x_cm) * SCALE;
                // y=0은 문 두께 중앙, y 양수는 화면 위쪽입니다.
                const topPos = (CONFIG_CM.ELEV_DEPTH - y_cm + (CONFIG_CM.DOOR_THICKNESS / 2)) * SCALE;
                const isSelected = selectedDot === p.id ? 'selected-glow' : '';

                return (
                <div 
                  key={p.id} 
                  className={`passenger-dot ${theme} ${isSelected}`}
                  style={{ left: leftPos, top: topPos }}
                  onClick={(e) => {
                    e.stopPropagation();
                    setSelectedDot(p.id);
                  }}
                >

                  <div className="dot-core"></div>
                </div>
              );
            })}

            </FloorPlan>
          </div>
        </div>

        <div className="side-panel">
          
          <div className="control-panel">
            <h2>능동 안전 제어 (Door Control)</h2>
            <div className="status-box">
              <h3>현재 도어 상태</h3>
              <div className={`status-indicator ${doorState === 'OPEN' || doorState === 'OPENING' ? 'normal-open' : 'normal-close'}`}>
                {doorState === 'OPEN' && '🟢 DOOR OPEN (열림)'}
                {doorState === 'CLOSED' && '🔵 DOOR CLOSED (닫힘)'}
                {doorState === 'OPENING' && '⏳ DOOR OPENING (열리는 중)'}
                {doorState === 'CLOSING' && '⏳ DOOR CLOSING (닫히는 중)'}
              </div>

              <div className="command-display">
                최근 제어 명령 (door_command) : 
                <span className={doorCommand === 'HOLD' ? 'text-danger' : 'text-normal'}>
                  {doorCommand === null ? ' null (없음)' : ` ${doorCommand}`}
                </span>
              </div>

              <div className="control-buttons">

                {doorState === 'CLOSED' || doorState === 'CLOSING' ? (
                  <button onClick={() => handleDoorCommand('OPEN')} className="btn-open">강제 OPEN</button>
                ) : (
                  <button onClick={() => handleDoorCommand('HOLD')} className="btn-hold">강제 HOLD</button>
                )}
                <button onClick={() => handleDoorCommand('CLOSE')} className="btn-close">강제 CLOSE</button>
              </div>
            </div>
          </div>

          <div className="log-monitor">
            <h2>실시간 시스템 로그 (Terminal)</h2>
            <div className="terminal-window">
              {systemLogs.map((log, index) => (
                <p key={index} className={log.includes('FALL') || log.includes('HOLD') || log.includes('IMMOBILE') ? 'log-danger' : ''}>
                  {log}
                </p>
              ))}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

export default App;