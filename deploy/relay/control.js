'use strict';

const { randomBytes, timingSafeEqual } = require('node:crypto');
const { integer, now } = require('./transport');

const commands = new Set(['OPEN', 'HOLD', 'CLOSE']);
const terminalStatuses = new Set(['APPLIED', 'REJECTED', 'EXPIRED', 'CANCELLED', 'UNKNOWN']);
const controlId = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,80}$/.test(value);
const secret = value => typeof value === 'string' && /^[A-Za-z0-9_-]{32,256}$/.test(value);
const issueId = () => randomBytes(24).toString('hex');

function matches(value, expected) {
  if (typeof value !== 'string' || typeof expected !== 'string') return false;
  const a = Buffer.from(value), b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

function authorized(req, token) {
  return matches(req.headers.authorization, 'Bearer ' + token);
}

function exact(object, keys) {
  return object && typeof object === 'object' && !Array.isArray(object) &&
    Object.keys(object).length === keys.length && keys.every(k => Object.hasOwn(object, k));
}

function readControl(data, binary, maxBytes) {
  if (binary || data.length > maxBytes) return null;
  try {
    const value = JSON.parse(data.toString('utf8'));
    return value && typeof value === 'object' && !Array.isArray(value) ? value : null;
  } catch { return null; }
}

function isBrowserRequest(value) {
  return exact(value, ['type', 'client_request_id', 'door_command']) &&
    value.type === 'door_request' && controlId(value.client_request_id) && commands.has(value.door_command);
}

function utc(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/.test(value)) return NaN;
  const parsed = Date.parse(value);
  return Number.isFinite(parsed) && new Date(parsed).toISOString() === value ? parsed : NaN;
}

function inspectRequest(value, link, backend, ttlMs) {
  if (!exact(value, ['type', 'request_id', 'control_session_id', 'backend_session_id',
    'operator_id', 'door_command', 'issued_at', 'expires_at']) || value.type !== 'door_request' ||
    !controlId(value.request_id) || !controlId(value.operator_id) || !commands.has(value.door_command)) return 'INVALID_COMMAND';
  if (!controlId(link) || !controlId(backend) ||
      value.control_session_id !== link || value.backend_session_id !== backend) return 'SESSION_CHANGED';
  const issued = utc(value.issued_at), expires = utc(value.expires_at);
  if (!Number.isFinite(issued) || !Number.isFinite(expires) ||
      expires <= issued || expires - issued > ttlMs) return 'INVALID_DEADLINE';
  return null;
}

function validScope(value, request) {
  return value.request_id === request.request_id && value.control_session_id === request.control_session_id &&
    value.backend_session_id === request.backend_session_id;
}

function isValidation(value, request) {
  return value.type === 'control_validate' && exact(value, ['type', 'request_id', 'control_session_id',
    'backend_session_id']) && validScope(value, request);
}

function isValidity(value, request, ttlMs) {
  return value.type === 'control_validity' && exact(value, ['type', 'request_id', 'control_session_id',
    'backend_session_id', 'remaining_ms']) && validScope(value, request) &&
    Number.isSafeInteger(value.remaining_ms) && value.remaining_ms >= 0 && value.remaining_ms <= ttlMs;
}

function remainingControlMs(deadline, expires, mono = now(), wall = Date.now()) {
  return Math.max(0, Math.floor(Math.min(deadline - mono, expires - wall)));
}

function validReady(value) {
  return value?.type === 'control_ready' && typeof value.enabled === 'boolean' &&
    typeof value.ready === 'boolean' && typeof value.backend_connected === 'boolean' && (value.reason === null || controlId(value.reason)) &&
    (value.backend_session_id === null || controlId(value.backend_session_id)) &&
    (!value.ready || (value.enabled && value.backend_connected && value.reason === null && controlId(value.backend_session_id)));
}

function validResult(value, request) {
  return value?.type === 'door_result' && value.request_id === request.request_id &&
    value.door_command === request.door_command &&
    value.control_session_id === request.control_session_id &&
    value.backend_session_id === request.backend_session_id &&
    terminalStatuses.has(value.status) && (value.reason === null || controlId(value.reason));
}

function result(request, status, reason = null) {
  return {
    type: 'door_result', request_id: request.request_id, control_session_id: request.control_session_id,
    backend_session_id: request.backend_session_id, door_command: request.door_command, status, reason
  };
}

function sendControl(ws, payload, maxBytes) {
  if (!ws || ws.readyState !== 1) return false;
  const text = JSON.stringify(payload);
  if (Buffer.byteLength(text) > maxBytes) return false;
  if (ws.bufferedAmount + Buffer.byteLength(text) > maxBytes * 4) {
    ws.terminate();
    return false;
  }
  try {
    ws.send(text, error => { if (error) ws.terminate(); });
    return true;
  } catch { ws.terminate(); return false; }
}

function controlLimits(config) {
  if (!config) return null;
  const maxBytes = integer(config.maxBytes, 1024, 'CONTROL_MAX_BYTES', 512);
  const ttlMs = integer(config.ttlMs, 2000, 'CONTROL_TTL_MS', 251);
  const requestsPerSecond = integer(config.requestsPerSecond, 5, 'CONTROL_RATE');
  if (maxBytes > 4096 || ttlMs > 10000 || requestsPerSecond > 30)
    throw new Error('Invalid control limits');
  return { maxBytes, ttlMs, requestsPerSecond };
}

module.exports = {
  authorized, controlId, controlLimits, exact, inspectRequest, isBrowserRequest, issueId,
  isValidation, isValidity, matches, readControl, remainingControlMs, result, secret, sendControl, validReady, validResult
};
