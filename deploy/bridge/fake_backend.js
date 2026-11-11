// 시험용 가짜 백엔드: 실제 백엔드 Mock 대신 A-D JSON v6 형식을 10Hz로 보냄
const { WebSocketServer } = require('ws');

const PORT = Number(process.env.PORT || 18081);
const wss = new WebSocketServer({ host: '127.0.0.1', port: PORT, path: '/radar' });
const start = Date.now();

setInterval(() => {
  const t = (Date.now() - start) / 1000; // 시작 후 경과 초
  const snapshot = {
    timestamp: new Date().toISOString(),
    timestamp_us: Math.round(t * 1e6),
    door_command: null,
    door_state: 'CLOSED',
    passengers: [
      // 캐빈 안에서 좌우로 천천히 움직이는 승객 1명 (화면이 살아 있는지 눈으로 확인용)
      { id: 1, x: 0.4 * Math.sin(t / 2), y: 0.8, state: 'INSIDE', safety_state: null, label: 'TEST #1' },
    ],
  };
  const text = JSON.stringify(snapshot);
  for (const client of wss.clients) {
    if (client.readyState === 1) client.send(text); // 1 = OPEN
  }
}, 100); // 100ms = 10Hz (A-D 합의 목표 주기)

console.log(`[fake] ws://127.0.0.1:${PORT}/radar`);