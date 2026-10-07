import React, { useState, useEffect } from 'react';
import './App.css';

// 🛠️ [실제 도면 규격 설정 - 단위: cm]
// 이제 px 단위로 머리 아프게 고민할 필요 없이 실제 줄자로 잰 cm 길이를 넣으세요!
const CONFIG_CM = {
  ELEV_WIDTH: 200,      // 카 폭 (가로) : 예) 200cm
  ELEV_DEPTH: 150,      // 카 깊이 (세로) : 예) 150cm
  DOOR_WIDTH: 90,       // 출입구 폭 : 예) 90cm
  WAITING_DEPTH: 180,   // 승강장(대기 구역) 길이 : 예) 180cm
};

// 🔍 화면 확대/축소 비율 (1cm = 몇 px로 그릴 것인가?)
// 도면이 화면에 비해 너무 작으면 이 숫자를 2.5, 3.0으로 늘리고, 너무 크면 1.5 등으로 줄이세요.
const SCALE = 1.5;

function App() {
  const [doorStatus, setDoorStatus] = useState('DOOR_HOLD');
  const [selectedDot, setSelectedDot] = useState(null); // 클릭한 승객 말풍선 띄우기용
  
  /* 
    =================================================================
    [데이터 수집 및 전처리 팀 연동 가이드]
    실제 4D 레이더 데이터 팀이 WebSocket이나 API로 넘겨줄 JSON 데이터 구조입니다.
    Expected Format:
      "timestamp": "2026-10-01T14:02:05Z",
      "door_command": "HOLD", -> null, OPEN, HOLD, CLOSE
      "door_state": "OPEN", -> OPEN, CLOSED, OPENING, CLOSEING
      "passengers": [
        { "id": 1, "x": -0.20, "y": 1.00, "state": "INSIDE","safety_state": null,"label": "Passenger #1"},
        { "id": 2, "x": 150, "y": 320, "state": "ENTERING", "safety_state": "FALL","label": "Passenger #2"}
         ->가능한 값 INSIDE, ENTERING, EXITING/IMMOBILE, FALL
      ]
    }
    =================================================================
  */

  // ---> [지워야함] 현재 테스트를 위해 임의로 박아둔 예시 승객 데이터입니다.
  // x, y 좌표는 도면 안에서의 위치입니다 (0,0 은 도면 좌측 상단 끝)
  // ---> [지워야함] 현재 테스트용 예시 승객 (x, y 좌표도 cm 단위입니다)
  // 초록점과 빨간점이 살짝 겹치도록 좌표를 비슷하게 두어, 빨간점이 무조건 위로 올라오는지 테스트합니다.
  const [passengers, setPassengers] = useState([
    { id: 1, x: -30, y: 100, state: 'INSIDE', safety_state: null, label: 'Passenger #1' },  // 정상 승객
    { id: 2, x: 40, y: -50, state: 'ENTERING', safety_state: 'FALL', label: 'Passenger #2' },    // 승강장 좌측 대기 중 낙상 발생 ->나중에 state를 OUTSIDE로 변경 상의중
    { id: 3, x: 80, y: 130, state: 'INSIDE', safety_state: 'IMMOBILE', label: 'Passenger #3' } // 카빈 내부 좌측 구석 장기 미동
  ]);

  const [systemLogs, setSystemLogs] = useState([
    '[14:01:00] RETINA-4SN Radar 연결 대기 중...',
    '[14:02:05] [예시 모드 작동 중] 실제 데이터 연동 전 UI 테스트 상태입니다.'
  ]);

  /* 
    =================================================================
    [실제 넣어야 할 부분 - WebSocket 연동 영역]
    =================================================================
  */
  /*
    // --- [실제 넣어야 함] 시작 ---
    useEffect(() => {
      const ws = new WebSocket('ws://localhost:8080/radar'); 
      
      ws.onmessage = (event) => {
        const realData = JSON.parse(event.data);
        setDoorStatus(realData.door_status); 
        setPassengers(realData.passengers); 
      };

      return () => ws.close();
    }, []);
    // --- [실제 넣어야 함] 끝 ---
  */

// 💡 safety_state 유무에 따라 화면 렌더링 색상 분기
  const getTheme = (safetyState) => {
    if (safetyState === 'FALL' || safetyState === 'IMMOBILE') return 'danger';
    return 'normal';
  };

  // 💡 상태값에 따라 말풍선에 띄울 한글 설명을 결정하는 함수
  const getStatusLabel = (state, safetyState) => {
    if (safetyState === 'FALL') return '🚨 비상 (낙상 감지!)';
    if (safetyState === 'IMMOBILE') return '🚨 비상 (장기 미동 감지!)';
    
    switch (state) {
      case 'ENTERING': return '🚶 입장 중 (정상)';
      case 'INSIDE': return '✅ 정상 탑승';
      case 'EXITING': return '🚶 퇴장 중 (정상)';
      default: return '알 수 없음';
    }
  };

  const handleManualControl = (status) => {
    setDoorStatus(status);
    const timeStr = new Date().toLocaleTimeString();
    setSystemLogs(prev => [
      ...prev,
      `[${timeStr}] [수동 제어] 도어 상태 변경: ${status}`
    ]);
  };

  // 배경 클릭 시 말풍선 닫기
  const handleBackgroundClick = () => {
    setSelectedDot(null);
  };

  return (
    <div className="dashboard-container" onClick={handleBackgroundClick}>
      <header className="header">
        <h1>ElevSafe 관제 시스템 — [RETINA-4SN 4D Radar 연동]</h1>
        <div className="spec-badge">
          <span>Range: 12m(Person) / 7m×7m</span> | <span>FoV: 90°(±45°)</span> | <span>Rate: 50ms</span>
        </div>
      </header>

      <div className="main-content">
        
        {/* 좌측: 2D 도면 (Blueprint) 시각화 영역 */}
        <div className="visualization-section">
          <h2>엘리베이터 및 승강장 2D 평면도 (1cm = {SCALE}px Scale)</h2>
          
          <div className="canvas-container">
            
            {/* SCALE을 곱해서 실제 화면 픽셀 크기로 자동 변환 */}
            <div className="blueprint-wrapper" style={{ width: CONFIG_CM.ELEV_WIDTH * SCALE }}> 
             
              {/* 1. 카 내부 (Cabin) */}
              <div 
                className="blueprint-cabin" 
                style={{ 
                  width: CONFIG_CM.ELEV_WIDTH * SCALE, 
                  height: CONFIG_CM.ELEV_DEPTH * SCALE 
                }}
              >
                <span className="cad-label">카 내부 (Cabin)</span>
              </div>

              {/* 2. 출입구 (Door Zone) */}
              <div className="blueprint-doors" style={{ width: CONFIG_CM.ELEV_WIDTH * SCALE }}>
                <div className="door-frame" style={{ width: CONFIG_CM.DOOR_WIDTH * SCALE }}>
                  <div className={`door left-door ${doorStatus}`}></div>
                  <div className={`door right-door ${doorStatus}`}></div>
                </div>
              </div>

              {/* 3. 승강장 대기 구역 (Waiting Area) */}
              <div 
                className="blueprint-waiting" 
                style={{ 
                  width: CONFIG_CM.ELEV_WIDTH * SCALE, 
                  height: CONFIG_CM.WAITING_DEPTH * SCALE 
                }}
              >
                <span className="cad-label">승강장 (Waiting Area)</span>
              </div>

              {/* 4. 승객(점) 렌더링 및 말풍선 툴팁 */}
              {passengers.map(p => {
                const theme = getTheme(p.safety_state);

                // [좌표 변환 핵심 로직]
                // X축: 중앙에서 카 내부를 기준으로 왼쪽(+)이면 화면 우측으로 더함
                const leftPos = (CONFIG_CM.ELEV_WIDTH / 2 + p.x) * SCALE;
                // Y축: 도면의 문턱 위치(ELEV_DEPTH)에서 Y값이 양수(+)면 위로 빼주고, 음수(-)면 아래로 더해짐
                const topPos = (CONFIG_CM.ELEV_DEPTH - p.y) * SCALE;


                return (
                <div 
                  key={p.id} 
                  className={`passenger-dot ${theme}`}
                  style={{ left: leftPos, top: topPos }}
                  onClick={(e) => {
                    e.stopPropagation();
                    setSelectedDot(p.id);
                  }}
                >
                  {/* 점 안의 코어 색상 */}
                  <div className="dot-core"></div>

                  {/* 점을 클릭했을 때 뜨는 말풍선 */}
                  {selectedDot === p.id && (
                    <div className={`dot-tooltip ${theme === 'danger' ? 'alert' : ''}`}>
                      <strong>{p.label}</strong>
                      <p>실제 좌표: (X: {p.x}cm, Y: {p.y}cm)</p>
                      <p className="state-text">
                        상태: {getStatusLabel(p.state, p.safety_state)}
                      </p>
                    </div>
                  )}
                </div>
              );
            })}

            </div>
          </div>
        </div>

        {/* 우측: 제어 및 로그 패널 */}
        <div className="side-panel">
          
          <div className="control-panel">
            <h2>능동 안전 제어 (Door Control)</h2>
            <div className="status-box">
              <h3>현재 도어 상태</h3>
              <div className={`status-indicator ${doorStatus === 'DOOR_HOLD' ? 'alert-hold' : 'normal-close'}`}>
                {doorStatus === 'DOOR_HOLD' && '🔴 DOOR HOLD (강제 개방 중)'}
                {doorStatus === 'DOOR_OPEN' && '🟢 DOOR OPEN'}
                {doorStatus === 'DOOR_CLOSE' && '🔵 DOOR CLOSE'}
              </div>
              
              <div className="control-buttons">
                <button onClick={() => handleManualControl('DOOR_HOLD')} className="btn-hold">HOLD 강제</button>
                <button onClick={() => handleManualControl('DOOR_CLOSE')} className="btn-close">CLOSE 강제</button>
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