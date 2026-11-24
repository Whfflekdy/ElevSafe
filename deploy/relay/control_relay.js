'use strict';

const { WebSocketServer } = require('ws');
const { now } = require('./transport');
const {
  authorized, controlLimits, inspectRequest, isBrowserRequest, issueId, isValidation,
  readControl, remainingControlMs, secret, sendControl, validReady, validResult
} = require('./control');

function relayControl(config) {
  const { maxBytes, ttlMs, requestsPerSecond } = controlLimits(config);
  const { operatorToken, bridgeToken, origin, trustProxy = false } = config;
  if (!secret(operatorToken) || !secret(bridgeToken) || operatorToken === bridgeToken ||
      typeof origin !== 'string' || new URL(origin).origin !== origin || !origin.startsWith('https://'))
    throw new Error('Invalid control authentication configuration');

  const operators = new WebSocketServer({ noServer: true, maxPayload: maxBytes, perMessageDeflate: false });
  const links = new WebSocketServer({ noServer: true, maxPayload: maxBytes, perMessageDeflate: false });
  const sessions = new Map();
  const loginAttempts = [];
  const cookieName = '__Secure-elevsafe-control';
  const sessionLifetime = 15 * 60 * 1000;
  const unavailable = () => ({ type: 'control_ready', enabled: true, backend_connected: false, ready: false,
    reason: 'BACKEND_UNAVAILABLE', backend_session_id: null });
  let link = null, linkId = null, readiness = unavailable(), pending = null;

  const secure = req => req.socket.encrypted || (trustProxy && req.headers['x-forwarded-proto'] === 'https');
  const originAllowed = req => req.headers.origin === origin;
  const send = (ws, object) => sendControl(ws, object, maxBytes);

  function cookie(req) {
    const pair = (req.headers.cookie || '').split(';').map(item => item.trim())
      .find(item => item.startsWith(cookieName + '='));
    return pair?.slice(cookieName.length + 1);
  }

  function authorizeOperator(req) {
    const id = cookie(req);
    const entry = sessions.get(id);
    return entry && entry.deadline > now() ? id : null;
  }

  function revoke(id) {
    sessions.delete(id);
    for (const ws of operators.clients) {
      if (ws.operatorSession === id) {
        releaseOperator(ws);
        ws.close(1008, 'Operator session ended');
      }
    }
  }

  function sweep() {
    for (const [id, entry] of sessions) {
      if (entry.deadline <= now()) revoke(id);
    }
  }

  function handleHttp(req, res) {
    if (!['/control/login', '/control/logout'].includes(req.url)) return false;
    res.setHeader('Cache-Control', 'no-store');
    if (req.method !== 'POST' || !secure(req) || !originAllowed(req)) {
      res.writeHead(403).end();
      return true;
    }
    if (req.url === '/control/logout') {
      const existing = authorizeOperator(req);
      if (existing) revoke(existing);
      res.setHeader('Set-Cookie', cookieName + '=; Path=/control; Secure; HttpOnly; SameSite=Strict; Max-Age=0');
      res.writeHead(204).end();
      return true;
    }
    const time = now();
    while (loginAttempts.length && time - loginAttempts[0] >= 60000) loginAttempts.shift();
    if (loginAttempts.length >= 20) { res.writeHead(429).end(); return true; }
    loginAttempts.push(time);
    if (!authorized(req, operatorToken)) { res.writeHead(403).end(); return true; }
    sweep();
    const previous = authorizeOperator(req);
    if (previous) revoke(previous);
    if (sessions.size >= 16) { res.writeHead(429).end(); return true; }
    const id = issueId();
    sessions.set(id, { deadline: now() + sessionLifetime, seen: new Set(), attempts: [] });
    res.setHeader('Set-Cookie',
      cookieName + '=' + id + '; Path=/control; Secure; HttpOnly; SameSite=Strict; Max-Age=' + sessionLifetime / 1000);
    res.writeHead(204).end();
    return true;
  }

  function browserResult(ws, clientId, requestId, command, status, reason = null) {
    return send(ws, { type: 'door_result', client_request_id: clientId, request_id: requestId,
      door_command: command, status, reason });
  }

  function finishPending(status, reason) {
    if (!pending) return;
    const { request, operator, clientId, timer } = pending;
    pending = null;
    clearTimeout(timer);
    browserResult(operator, clientId, request.request_id, request.door_command, status, reason);
  }

  function releaseOperator(ws) {
    if (ws.operatorRevoked) return;
    ws.operatorRevoked = true;
    if (!link || ws.requestLink !== linkId) return;
    if (pending?.operator === ws) pending.cancelled = true;
    if (!send(link, { type: 'control_revoke', control_session_id: linkId, operator_id: ws.operatorId }))
      link.terminate();
  }

  function cancelPending() {
    if (!pending || pending.cancelled) return;
    pending.cancelled = true;
    const { request_id, control_session_id, backend_session_id } = pending.request;
    if (!send(link, { type: 'control_cancel', request_id, control_session_id, backend_session_id })) link?.terminate();
  }

  function publishReady(value) {
    readiness = value;
    for (const ws of operators.clients) send(ws, readiness);
  }

  function acceptOperator(ws, id) {
    ws.operatorSession = id;
    ws.operatorId = issueId();
    send(ws, readiness);
    ws.on('close', () => releaseOperator(ws));
    ws.on('message', (data, binary) => {
      if (ws.operatorRevoked) return;
      const entry = sessions.get(id);
      if (!entry || entry.deadline <= now()) { revoke(id); return; }
      const msg = readControl(data, binary, maxBytes);
      const clientId = typeof msg?.client_request_id === 'string' &&
        /^[A-Za-z0-9_-]{1,80}$/.test(msg.client_request_id) ? msg.client_request_id : null;
      const reject = reason => browserResult(ws, clientId, null, isBrowserRequest(msg) ? msg.door_command : null,
        'REJECTED', reason);
      const time = now();
      while (entry.attempts.length && time - entry.attempts[0] >= 1000) entry.attempts.shift();
      if (entry.attempts.length >= requestsPerSecond) { reject('RATE_LIMIT'); return; }
      entry.attempts.push(time);
      if (!isBrowserRequest(msg)) { reject('INVALID_COMMAND'); return; }
      if (entry.seen.has(clientId)) { reject('DUPLICATE_REQUEST'); return; }
      // Leave existing IDs intact until this user session expires.
      if (entry.seen.size >= 4096) { reject('SESSION_LIMIT'); return; }
      entry.seen.add(clientId);
      if (!link || !readiness.ready) { reject(readiness.reason || 'BACKEND_UNAVAILABLE'); return; }
      if (pending) { reject('QUEUE_FULL'); return; }
      const issued = Date.now(), expires = issued + ttlMs;
      const request = { type: 'door_request', request_id: issueId(), control_session_id: linkId,
        backend_session_id: readiness.backend_session_id, operator_id: ws.operatorId, door_command: msg.door_command,
        issued_at: new Date(issued).toISOString(), expires_at: new Date(expires).toISOString() };
      const problem = inspectRequest(request, linkId, readiness.backend_session_id, ttlMs);
      if (problem || !send(link, request)) { reject(problem || 'BACKEND_UNAVAILABLE'); return; }
      ws.requestLink = linkId;
      pending = { request, operator: ws, clientId, expires, deadline: Math.min(time + ttlMs, entry.deadline),
        timer: setTimeout(() => {
          cancelPending();
          finishPending('UNKNOWN', 'RESULT_TIMEOUT');
          // Fence the link before accepting a new request after a missing outcome.
          link?.terminate();
          publishReady(unavailable());
        }, ttlMs + 500) };
      if (!browserResult(ws, clientId, request.request_id, request.door_command, 'ACCEPTED')) releaseOperator(ws);
    });
  }

  function acceptLink(ws) {
    link = ws;
    linkId = issueId();
    send(ws, { type: 'control_link', control_session_id: linkId });
    ws.on('close', () => {
      if (link !== ws) return;
      link = null;
      linkId = null;
      finishPending('UNKNOWN', 'CONTROL_LINK_LOST');
      publishReady(unavailable());
    });
    ws.on('message', (data, binary) => {
      if (ws !== link) return;
      const msg = readControl(data, binary, maxBytes);
      if (!msg) { ws.close(1008, 'Invalid control payload'); return; }
      if (validReady(msg)) publishReady({ type: 'control_ready', enabled: msg.enabled,
        ready: msg.ready, backend_connected: msg.backend_connected, reason: msg.reason, backend_session_id: msg.backend_session_id });
      else if (pending && validResult(msg, pending.request)) finishPending(msg.status, msg.reason);
      else if (msg.type === 'control_validate') {
        if (!pending || pending.validated || !isValidation(msg, pending.request)) return;
        pending.validated = true;
        const remaining = pending.cancelled ? 0 : remainingControlMs(pending.deadline, pending.expires);
        if (!send(ws, { ...msg, type: 'control_validity', remaining_ms: remaining })) ws.terminate();
      } else if (msg.type !== 'door_result') ws.close(1008, 'Invalid control message');
    });
  }

  function upgrade(req, socket, head, url, attach, reject) {
    if (url.pathname !== '/control' && url.pathname !== '/control-link') return false;
    if (req.url !== url.pathname || !secure(req)) { reject(socket, '403 Forbidden'); return true; }
    if (url.pathname === '/control') {
      const id = originAllowed(req) && authorizeOperator(req);
      if (!id) reject(socket, '403 Forbidden');
      else if (operators.clients.size >= 16) reject(socket, '429 Too Many Requests');
      else operators.handleUpgrade(req, socket, head, ws => { attach(ws); acceptOperator(ws, id); });
    } else if (!authorized(req, bridgeToken)) reject(socket, '403 Forbidden');
    else if (link) reject(socket, '409 Conflict');
    else links.handleUpgrade(req, socket, head, ws => { attach(ws); acceptLink(ws); });
    return true;
  }

  function stop() {
    cancelPending();
    finishPending('UNKNOWN', 'RELAY_STOPPING');
    sessions.clear();
  }

  return { servers: [operators, links], handleHttp, upgrade, sweep, stop };
}

module.exports = { relayControl };
