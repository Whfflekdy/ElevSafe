// 노트북(입구 /ingest)에서 받은 JSON을 보는 화면들(출구 /radar)에 그대로 나눠 줌
const http = require('http');
const { WebSocketServer, WebSocket } = require('ws');

const PORT = Number(process.env.PORT || 8090);
const INGEST_KEY = process.env.INGEST_KEY;

// INGEST_KEY가 비어 있으면 에러를 출력하고 종료하기
if (!INGEST_KEY) { console.error('INGEST_KEY is not set'); process.exit(1); }

// 상태 확인용: 브라우저로 열면 ok
const server = http.createServer((req, res) => res.end('ok'));
const viewers = new WebSocketServer({ noServer: true });
const ingest = new WebSocketServer({ noServer: true });
let lastSnapshot = null; // 새로 들어온 화면에 바로 보여 줄 마지막 데이터

server.on('upgrade', (req, socket, head) => {
    const url = new URL(req.url, 'http://localhost');
    // 주소의 key 값이 INGEST_KEY와 같을 때만 true
    const key = url.searchParams.get('key');
    const keyOk = key === INGEST_KEY;

    if (url.pathname === '/radar') {
        viewers.handleUpgrade(req, socket, head, (ws) => viewers.emit('connection', ws));
    } else if (url.pathname === '/ingest' && keyOk) {
        ingest.handleUpgrade(req, socket, head, (ws) => ingest.emit('connection', ws));
    } else {
        if (url.pathname === '/ingest') console.log('[relay] 키가 틀린 노트북 연결 거부');
        socket.destroy();
    }
});

ingest.on('connection', (ws) => {
    console.log('[relay] 노트북 연결됨');
    // TODO 3 (나중에): 노트북 연결은 하나만 허용
    ws.on('message', (data) => {
        lastSnapshot = data.toString();
        for (const viewer of viewers.clients) {
            if (viewer.readyState !== WebSocket.OPEN) continue;
            // TODO 4 (나중에): viewer.bufferedAmount가 크면 이번 프레임 건너뛰기
            viewer.send(lastSnapshot);
        }
    });
    ws.on('close', () => console.log('[relay] 노트북 연결 끊김'));
});

viewers.on('connection', (ws) => {
    if (lastSnapshot) ws.send(lastSnapshot);
    // 보기 전용: 화면이 보낸 문 제어 명령은 버림
    ws.on('message', () => { });
});

server.listen(PORT, () => console.log(`[relay] listening :${PORT}`));