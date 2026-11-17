// 노트북 백엔드(로컬)에서 받은 JSON을 학교 서버 /ingest로 그대로 보냄
const WebSocket = require('ws');

const LOCAL_URL = process.env.LOCAL_URL || 'ws://127.0.0.1:8080/radar';
const REMOTE_URL = process.env.REMOTE_URL; // 예: ws://서버주소:18080/ingest?key=...
// TODO 6 (나중에): REMOTE_URL이 없으면 사용법 출력하고 종료

let remote = null;

function connectRemote() {
  remote = new WebSocket(REMOTE_URL);
  remote.on('open', () => console.log('[bridge] 서버 연결됨'));
  remote.on('close', () => {
    console.log('[bridge] 서버 끊김, 2초 뒤 재연결');
    setTimeout(connectRemote, 2000);
  });
  remote.on('error', () => {});
}

function connectLocal() {
  const local = new WebSocket(LOCAL_URL);
  local.on('open', () => console.log('[bridge] 로컬 백엔드 연결됨'));
  local.on('message', (data) => {
    // 서버가 끊겨 있으면 그냥 버림: 현장 처리에는 영향 없음
    if (remote && remote.readyState === WebSocket.OPEN) remote.send(data.toString());
  });
  local.on('close', () => {
    console.log('[bridge] 로컬 끊김, 2초 뒤 재연결');
    setTimeout(connectLocal, 2000);
  });
  local.on('error', () => {});
}

connectRemote();
connectLocal();