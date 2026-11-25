'use strict';

const WebSocket = require('ws');
const { now } = require('../relay/transport');
const { controlId, controlLimits, exact, inspectRequest, isValidation, isValidity, readControl, result,
  secret, sendControl, validReady, validResult } = require('../relay/control');

function controlUrl(value, remote, allowInsecureDemo = false) {
  const url = new URL(value);
  if (url.username || url.password || url.search || url.hash ||
      (remote ? (url.protocol !== 'wss:' && !(allowInsecureDemo === true && url.protocol === 'ws:')) || url.pathname !== '/control-link'
        : url.protocol !== 'ws:' || url.pathname !== '/control-local' ||
          !['127.0.0.1', '[::1]'].includes(url.hostname))) throw new Error('Invalid control URL');
  return url.href;
}

function createControlBridge(config, { reconnectMs, connectTimeoutMs, heartbeatMs, log }) {
  const { maxBytes, ttlMs } = controlLimits(config);
  if (!secret(config.bridgeToken) || !secret(config.localToken) || config.bridgeToken === config.localToken)
    throw new Error('Invalid Bridge control authentication');
  const remote = { name: 'control-link', url: controlUrl(config.remoteUrl, true, config.allowInsecureDemo), key: config.bridgeToken };
  const local = { name: 'control-local', url: controlUrl(config.localUrl, false), key: config.localToken };
  let stopping = false, stopPromise, linkId = null, backend = null, pending = null;
  const notReady = () => ({ type: 'control_ready', enabled: true, backend_connected: false, ready: false,
    reason: 'BACKEND_UNAVAILABLE', backend_session_id: null });
  const send = (side, msg) => sendControl(side.ws, msg, maxBytes);
  const resetLocal = () => {
    backend = null;
    const ws = local.ws;
    if (ws) ws.terminate();
  };

  function localMessage(msg) {
    if (validReady(msg)) {
      backend = msg;
      send(remote, msg);
    } else if (pending && validResult(msg, pending.request)) {
      pending = null;
      send(remote, msg);
    } else if (pending && isValidation(msg, pending.request) && pending.validationSent === undefined) {
      pending.validationSent = now();
      if (!send(remote, msg)) resetLocal();
    } else local.ws?.close(1008, 'Invalid Backend control message');
  }

  function remoteMessage(msg) {
    if (msg.type === 'control_link' && exact(msg, ['type', 'control_session_id']) &&
        controlId(msg.control_session_id) && !linkId) {
      linkId = msg.control_session_id;
      connect(local);
      return;
    }
    if (msg.type === 'control_revoke') {
      if (linkId && exact(msg, ['type', 'control_session_id', 'operator_id']) &&
          msg.control_session_id === linkId && controlId(msg.operator_id) && !send(local, msg))
        resetLocal();
      return;
    }
    if (msg.type === 'control_validity') {
      if (pending?.validationSent !== undefined && isValidity(msg, pending.request, ttlMs)) {
        if (now() - pending.validationSent >= msg.remaining_ms) msg.remaining_ms = 0;
        if (!send(local, msg)) resetLocal();
      }
      return;
    }
    if (msg.type === 'control_cancel') {
      if (pending && exact(msg, ['type', 'request_id', 'control_session_id', 'backend_session_id']) &&
          msg.request_id === pending.request.request_id && msg.control_session_id === linkId &&
          msg.backend_session_id === pending.request.backend_session_id) {
        if (!send(local, msg)) resetLocal();
      }
      return;
    }
    const problem = inspectRequest(msg, linkId, backend?.backend_session_id, ttlMs);
    if (problem || !backend?.ready || pending) {
      const reason = problem || (pending ? 'QUEUE_FULL' : 'BACKEND_UNAVAILABLE');
      send(remote, result(msg, 'REJECTED', reason));
      return;
    }
    if (send(local, msg)) pending = { request: msg };
    else send(remote, result(msg, 'REJECTED', 'BACKEND_UNAVAILABLE'));
  }

  function schedule(side) {
    if (stopping || side.timer || (side === local && !linkId)) return;
    side.timer = setTimeout(() => { side.timer = null; connect(side); }, reconnectMs);
  }

  function connect(side) {
    if (stopping || side.ws || (side === local && !linkId)) return;
    let ws;
    try {
      ws = new WebSocket(side.url, {
        headers: { Authorization: 'Bearer ' + side.key }, handshakeTimeout: connectTimeoutMs,
        maxPayload: maxBytes, perMessageDeflate: false,
        ...(side === remote ? { ca: config.ca, rejectUnauthorized: true } : {})
      });
    } catch { schedule(side); return; }
    side.ws = ws;
    ws.alive = true;
    ws.on('pong', () => { ws.alive = true; });
    ws.on('error', () => { ws.terminate(); });
    ws.on('open', () => {
      if (stopping || side.ws !== ws) return;
      log('[bridge] ' + side.name + ' connected');
      if (side === local && (!linkId || !send(local, {
        type: 'control_bind', control_session_id: linkId
      }))) ws.terminate();
    });
    ws.on('message', (data, binary) => {
      if (stopping || side.ws !== ws) return;
      const msg = readControl(data, binary, maxBytes);
      if (!msg) { ws.close(1008, 'Invalid control payload'); return; }
      if (side === local) localMessage(msg);
      else remoteMessage(msg);
    });
    ws.on('close', () => {
      if (side.ws !== ws) return;
      side.ws = null;
      log('[bridge] ' + side.name + ' disconnected');
      if (side === remote) {
        linkId = null;
        pending = null;
        clearTimeout(local.timer);
        local.timer = null;
        resetLocal();
      } else {
        backend = null;
        if (pending) { send(remote, result(pending.request, 'UNKNOWN', 'BACKEND_CONTROL_LOST')); pending = null; }
        send(remote, notReady());
      }
      schedule(side);
    });
  }

  const heartbeat = setInterval(() => {
    for (const side of [local, remote]) {
      const ws = side.ws;
      if (!ws || ws.readyState !== WebSocket.OPEN) continue;
      if (!ws.alive) { ws.terminate(); continue; }
      ws.alive = false;
      try { ws.ping(); } catch { ws.terminate(); }
    }
  }, heartbeatMs);
  connect(remote);

  function stop() {
    if (stopPromise) return stopPromise;
    stopping = true;
    clearInterval(heartbeat);
    stopPromise = Promise.all([local, remote].map(side => {
      clearTimeout(side.timer);
      const ws = side.ws;
      if (!ws || ws.readyState === WebSocket.CLOSED) return Promise.resolve();
      return new Promise(resolve => {
        const deadline = setTimeout(() => ws.terminate(), 500);
        ws.once('close', () => { clearTimeout(deadline); resolve(); });
        if (ws.readyState === WebSocket.OPEN) ws.close(1001, 'Bridge shutdown');
        else ws.terminate();
      });
    })).then(() => {});
    return stopPromise;
  }

  return { stop };
}

module.exports = { createControlBridge };
