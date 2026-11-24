'use strict';
const http = require('node:http');
const { timingSafeEqual } = require('node:crypto');
const { WebSocketServer } = require('ws');
const { integer, limits, snapshotText, sendBounded, installSignals, now } = require('./transport');

function createRelay(options = {}) {
  const key = options.key;
  if (typeof key !== 'string' || !key.length) throw new Error('INGEST_KEY is required');
  const port = integer(options.port, 8090, 'PORT', 0);
  if (port > 65535) throw new Error('Invalid PORT');
  const { maxMessageBytes, maxBufferedBytes } = limits(options);
  const staleMs = integer(options.staleMs, 3000, 'STALE_MS');
  const heartbeatMs = integer(options.heartbeatMs, 30000, 'HEARTBEAT_MS');
  const log = options.log || (() => {});
  const stats = { accepted: 0, invalid: 0, rejectedIngest: 0, slowViewers: 0, ignoredCommands: 0, errors: 0, expired: 0 };
  const server = http.createServer((req, res) => res.end('ok'));
  const viewers = new WebSocketServer({ noServer: true, maxPayload: maxMessageBytes, perMessageDeflate: false });
  const ingest = new WebSocketServer({ noServer: true, maxPayload: maxMessageBytes, perMessageDeflate: false });
  let producer = null, lastSnapshot = null, stopping = false, stopPromise;
  const sockets = new Set();
  server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
  function expire() {
    if (lastSnapshot && now() - lastSnapshot.receivedAt >= staleMs) { lastSnapshot = null; ++stats.expired; }
  }
  function attach(ws) {
    ws.alive = true;
    ws.on('pong', () => { ws.alive = true; });
    ws.on('error', () => { ++stats.errors; ws.terminate(); });
  }
  function broadcast(ws, text) {
    if (sendBounded(ws, text, maxBufferedBytes, () => ++stats.errors) === 'slow') ++stats.slowViewers;
  }
  function reject(socket, status) {
    socket.on('error', () => {});
    socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  }
  server.on('upgrade', (req, socket, head) => {
    if (stopping) return reject(socket, '503 Service Unavailable');
    let url;
    try { url = new URL(req.url, 'http://localhost'); } catch { return reject(socket, '400 Bad Request'); }
    if (url.pathname === '/radar') {
      viewers.handleUpgrade(req, socket, head, ws => viewers.emit('connection', ws));
    } else if (url.pathname === '/ingest') {
      const supplied = Buffer.from(url.searchParams.get('key') || '');
      const expected = Buffer.from(key);
      if (supplied.length !== expected.length || !timingSafeEqual(supplied, expected))
        return reject(socket, '403 Forbidden');
      if (producer) { ++stats.rejectedIngest; return reject(socket, '409 Conflict'); }
      ingest.handleUpgrade(req, socket, head, ws => {
        producer = ws; // Reserve synchronously; keep it until the close event completes.
        ingest.emit('connection', ws);
      });
    } else reject(socket, '404 Not Found');
  });
  ingest.on('connection', ws => {
    attach(ws); lastSnapshot = null; log('[relay] ingest connected');
    ws.on('message', (data, binary) => {
      if (stopping || ws !== producer) return;
      let text;
      try { text = snapshotText(data, binary, maxMessageBytes); }
      catch { ++stats.invalid; return; } // Invalid input does not refresh freshness.
      lastSnapshot = { text, receivedAt: now() };
      ++stats.accepted;
      for (const viewer of viewers.clients) broadcast(viewer, text);
    });
    ws.on('close', () => {
      if (producer === ws) { producer = null; lastSnapshot = null; }
      log('[relay] ingest disconnected; cache invalidated');
    });
  });
  viewers.on('connection', ws => {
    attach(ws); expire();
    if (lastSnapshot) broadcast(ws, lastSnapshot.text);
    ws.on('message', () => { ++stats.ignoredCommands; }); // Never route commands upstream.
  });
  const expiry = setInterval(expire, Math.min(staleMs, 1000));
  const heartbeat = setInterval(() => {
    for (const ws of [...viewers.clients, ...ingest.clients]) {
      if (!ws.alive) { ws.terminate(); continue; }
      ws.alive = false;
      try { ws.ping(); } catch { ws.terminate(); }
    }
  }, heartbeatMs);
  const ready = new Promise((resolve, rejectReady) => {
    server.once('listening', resolve);
    server.once('error', rejectReady);
  });
  server.on('error', () => { ++stats.errors; log('[relay] server error'); });
  server.listen(port, options.host || '0.0.0.0');
  function stop() {
    if (stopPromise) return stopPromise;
    stopping = true; lastSnapshot = null;
    clearInterval(expiry); clearInterval(heartbeat);
    stopPromise = new Promise(resolve => {
      const deadline = setTimeout(() => {
        for (const ws of [...viewers.clients, ...ingest.clients]) ws.terminate();
        for (const socket of sockets) socket.destroy();
      }, 500);
      const closed = [viewers, ingest, server].map(s => new Promise(done => s.close(() => done())));
      for (const ws of [...viewers.clients, ...ingest.clients]) ws.close(1001, 'Server shutdown');
      Promise.all(closed).then(() => { clearTimeout(deadline); resolve(); });
    });
    return stopPromise;
  }
  return { server, ready, stop, stats, cached: () => { expire(); return lastSnapshot !== null; } };
}
if (require.main === module) {
  let relay;
  try {
    relay = createRelay({ key: process.env.INGEST_KEY, port: process.env.PORT,
      staleMs: process.env.STALE_MS, maxMessageBytes: process.env.MAX_MESSAGE_BYTES,
      maxBufferedBytes: process.env.MAX_BUFFERED_BYTES, heartbeatMs: process.env.HEARTBEAT_MS, log: console.log });
    installSignals(relay.stop);
    relay.ready.then(() => console.log('[relay] listening'), async () => { await relay.stop(); process.exitCode = 1; });
  } catch { console.error('[relay] invalid configuration'); process.exitCode = 1; }
}
module.exports = { createRelay };
