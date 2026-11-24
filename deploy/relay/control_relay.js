'use strict';

const { WebSocketServer } = require('ws');
const { now } = require('./transport');
const {
  authorized, controlLimits, inspectRequest, isBrowserRequest, issueId, isValidation,
  readControl, remainingControlMs, secret, sendControl, validReady, validResult
} = require('./control');

function relayControl(config, audit) {
  const { maxBytes, ttlMs, requestsPerSecond } = controlLimits(config);
  const { bridgeToken } = config;
  if (!secret(bridgeToken)) throw new Error('Invalid Bridge control credential');
  const links = new WebSocketServer({ noServer: true, maxPayload: maxBytes, perMessageDeflate: false });
  const globalAttempts = [];
  const unavailable = () => ({ type: 'control_ready', enabled: true, backend_connected: false, ready: false,
    reason: 'BACKEND_UNAVAILABLE', backend_session_id: null });
  let link = null, linkId = null, readiness = unavailable(), pending = null;

  const send = (ws, object) => sendControl(ws, object, maxBytes);
  function finishPending(status, reason) {
    if (!pending) return;
    const { request, timer } = pending;
    pending = null;
    clearTimeout(timer);
    audit({ type: 'door_result', request_id: request.request_id, operator_id: request.operator_id,
      door_command: request.door_command, status, reason });
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
    audit(readiness);
  }

  function prune(attempts, time) {
    while (attempts.length && time - attempts[0] >= 1000) attempts.shift();
  }

  function acceptOperator(ws) {
    ws.operatorId = issueId();
    const attempts = [];
    ws.on('close', () => releaseOperator(ws));
    ws.on('message', (data, binary) => {
      if (ws.operatorRevoked) return;
      const time = now();
      prune(attempts, time);
      prune(globalAttempts, time);
      const reject = reason => audit({ type: 'door_result', request_id: null,
        operator_id: ws.operatorId, status: 'REJECTED', reason });
      if (attempts.length >= requestsPerSecond || globalAttempts.length >= requestsPerSecond * 4) {
        reject('RATE_LIMIT');
        releaseOperator(ws);
        ws.close(1008, 'Rate limit');
        return;
      }
      attempts.push(time);
      globalAttempts.push(time);
      const msg = readControl(data, binary, maxBytes);
      if (!isBrowserRequest(msg)) { reject('INVALID_COMMAND'); return; }
      if (!link || !readiness.ready) { reject(readiness.reason || 'BACKEND_UNAVAILABLE'); return; }
      if (pending) { reject('QUEUE_FULL'); return; }
      const issued = Date.now(), expires = issued + ttlMs;
      const request = { type: 'door_request', request_id: issueId(), control_session_id: linkId,
        backend_session_id: readiness.backend_session_id, operator_id: ws.operatorId, door_command: msg.door_command,
        issued_at: new Date(issued).toISOString(), expires_at: new Date(expires).toISOString() };
      const problem = inspectRequest(request, linkId, readiness.backend_session_id, ttlMs);
      if (problem || !send(link, request)) { reject(problem || 'BACKEND_UNAVAILABLE'); return; }
      ws.requestLink = linkId;
      pending = { request, operator: ws, expires, deadline: time + ttlMs,
        timer: setTimeout(() => {
          cancelPending();
          finishPending('UNKNOWN', 'RESULT_TIMEOUT');
          // Fence the link before accepting a new request after a missing outcome.
          link?.terminate();
          publishReady(unavailable());
        }, ttlMs + 500) };
      audit({ type: 'door_result', request_id: request.request_id, operator_id: ws.operatorId,
        door_command: request.door_command, status: 'ACCEPTED', reason: null });
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

  function upgrade(req, socket, head, attach, reject, secure) {
    if (!secure || !authorized(req, bridgeToken)) reject(socket, '403 Forbidden');
    else if (link) reject(socket, '409 Conflict');
    else links.handleUpgrade(req, socket, head, ws => { attach(ws); acceptLink(ws); });
    return true;
  }

  function stop() {
    cancelPending();
    finishPending('UNKNOWN', 'RELAY_STOPPING');

  }

  return { servers: [links], acceptOperator, upgrade, stop };
}

module.exports = { relayControl };
