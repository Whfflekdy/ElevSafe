'use strict';
const WebSocket = require('ws');
const { integer, limits, snapshotText, sendBounded, installSignals } = require('../relay/transport');
const { createControlBridge } = require('./control_bridge');
function websocketUrl(value, name) {
  let url;
  try { url = new URL(value); } catch { throw new Error(`Invalid ${name}`); }
  if (!['ws:', 'wss:'].includes(url.protocol) || !url.hostname || url.username || url.password || url.hash)
    throw new Error(`Invalid ${name}`);
  return url.href;
}
function createBridge(options = {}) {
  const localUrl = websocketUrl(options.localUrl ?? 'ws://127.0.0.1:8080/radar', 'LOCAL_URL');
  const remoteUrl = websocketUrl(options.remoteUrl, 'REMOTE_URL');
  const { maxMessageBytes, maxBufferedBytes } = limits(options);
  const reconnectMs = integer(options.reconnectMs, 2000, 'RECONNECT_MS');
  const connectTimeoutMs = integer(options.connectTimeoutMs, 5000, 'CONNECT_TIMEOUT_MS');
  const heartbeatMs = integer(options.heartbeatMs, 30000, 'HEARTBEAT_MS');
  const log = options.log || (() => {});
  const ingestKey = new URL(remoteUrl).searchParams.get('key');
  if (options.control && [options.control.localToken, options.control.bridgeToken].includes(ingestKey))
    throw new Error('Shared control and ingest credentials are invalid');
  const control = options.control && createControlBridge(options.control,
    { reconnectMs, connectTimeoutMs, heartbeatMs, log });
  const stats = { forwarded: 0, dropped: 0, invalid: 0, slowRemote: 0, errors: 0, localOpens: 0, remoteOpens: 0 };
  const local = { name: 'local', url: localUrl, socket: null, timer: null };
  const remote = { name: 'remote', url: remoteUrl, socket: null, timer: null };
  let stopping = false, stopPromise;
  function schedule(side) {
    if (stopping || side.timer) return;
    side.timer = setTimeout(() => { side.timer = null; connect(side); }, reconnectMs);
  }
  function connect(side) {
    if (stopping || side.socket) return;
    let socket;
    try {
      socket = new WebSocket(side.url, { handshakeTimeout: connectTimeoutMs,
        maxPayload: maxMessageBytes, perMessageDeflate: false, ca: side === remote ? options.ca : undefined });
    } catch { ++stats.errors; schedule(side); return; }
    side.socket = socket; socket.alive = true;
    socket.on('open', () => {
      ++stats[side === local ? 'localOpens' : 'remoteOpens'];
      log(`[bridge] ${side.name} connected`);
    });
    socket.on('pong', () => { socket.alive = true; });
    socket.on('error', () => { ++stats.errors; socket.terminate(); }); // No URL/key/error payload in logs.
    socket.on('close', () => {
      if (side.socket === socket) side.socket = null;
      log(`[bridge] ${side.name} disconnected`); schedule(side);
    });
    if (side === local) socket.on('message', (data, binary) => {
      if (stopping) return;
      let text;
      try { text = snapshotText(data, binary, maxMessageBytes); }
      catch { ++stats.invalid; return; }
      const result = sendBounded(remote.socket, text, maxBufferedBytes, () => ++stats.errors);
      if (result === 'sent') ++stats.forwarded;
      else { ++stats.dropped; if (result === 'slow') ++stats.slowRemote; }
    });
    // No remote message handler: remote commands cannot reach the local Backend.
  }
  const heartbeat = setInterval(() => {
    for (const side of [local, remote]) {
      const socket = side.socket;
      if (!socket || socket.readyState !== WebSocket.OPEN) continue;
      if (!socket.alive) { socket.terminate(); continue; }
      socket.alive = false;
      try { socket.ping(); } catch { socket.terminate(); }
    }
  }, heartbeatMs);
  connect(remote); connect(local);
  function stop() {
    if (stopPromise) return stopPromise;
    stopping = true; clearInterval(heartbeat);
    const closing = control ? [control.stop()] : [];
    for (const side of [local, remote]) {
      clearTimeout(side.timer); side.timer = null;
      const socket = side.socket;
      if (!socket || socket.readyState === WebSocket.CLOSED) continue;
      closing.push(new Promise(resolve => {
        const deadline = setTimeout(() => socket.terminate(), 500);
        socket.once('close', () => { clearTimeout(deadline); resolve(); });
        if (socket.readyState === WebSocket.OPEN) socket.close(1001, 'Bridge shutdown');
        else socket.terminate();
      }));
    }
    stopPromise = Promise.all(closing).then(() => {});
    return stopPromise;
  }
  return { stop, stats };
}
if (require.main === module) {
  try {
    const bridge = createBridge({ localUrl: process.env.LOCAL_URL, remoteUrl: process.env.REMOTE_URL,
      maxMessageBytes: process.env.MAX_MESSAGE_BYTES, maxBufferedBytes: process.env.MAX_BUFFERED_BYTES,
      reconnectMs: process.env.RECONNECT_MS, connectTimeoutMs: process.env.CONNECT_TIMEOUT_MS,
      heartbeatMs: process.env.HEARTBEAT_MS, log: console.log,
      control: process.env.CONTROL_ENABLED === '1' ? {
        allowInsecureDemo: process.env.CONTROL_ALLOW_INSECURE_DEMO === '1',
        remoteUrl: process.env.CONTROL_REMOTE_URL, bridgeToken: process.env.CONTROL_BRIDGE_TOKEN,
        localUrl: process.env.CONTROL_LOCAL_URL, localToken: process.env.CONTROL_LOCAL_TOKEN,
        maxBytes: process.env.CONTROL_MAX_BYTES, ttlMs: process.env.CONTROL_TTL_MS
      } : undefined });
    installSignals(bridge.stop);
  } catch { console.error('[bridge] invalid configuration (check LOCAL_URL / REMOTE_URL / limits)'); process.exitCode = 1; }
}
module.exports = { createBridge };
