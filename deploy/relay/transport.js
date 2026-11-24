'use strict';
// Transport limits shared by the relay and laptop bridge. No message rewriting.
const { performance } = require('node:perf_hooks');
function integer(value, fallback, name, min = 1) {
  const n = value === undefined ? fallback : Number(value);
  if (!Number.isSafeInteger(n) || n < min || n > 2147483647) throw new Error(`Invalid ${name}`);
  return n;
}
function limits(options = {}) {
  const maxMessageBytes = integer(options.maxMessageBytes, 65536, 'MAX_MESSAGE_BYTES');
  const maxBufferedBytes = integer(options.maxBufferedBytes, 131072, 'MAX_BUFFERED_BYTES');
  if (maxBufferedBytes < maxMessageBytes) throw new Error('MAX_BUFFERED_BYTES must cover one message');
  return { maxMessageBytes, maxBufferedBytes };
}
function snapshotText(data, binary, maxBytes) {
  if (binary || data.length > maxBytes) throw new Error('Invalid message type or size');
  const text = data.toString('utf8');
  const p = JSON.parse(text);
  const oneOf = (v, values) => values.includes(v);
  if (!p || Array.isArray(p) || typeof p !== 'object' || Object.keys(p).length !== 5 ||
      typeof p.timestamp !== 'string' || !Number.isInteger(p.timestamp_us) || p.timestamp_us < 0 ||
      !oneOf(p.door_state, ['UNKNOWN', 'OPEN', 'CLOSED', 'OPENING', 'CLOSING']) ||
      !oneOf(p.door_command, [null, 'OPEN', 'HOLD', 'CLOSE']) || !Array.isArray(p.passengers))
    throw new Error('Invalid snapshot');
  for (const v of p.passengers) {
    if (!v || Array.isArray(v) || typeof v !== 'object' ||
        !(typeof v.id === 'string' || Number.isInteger(v.id)) ||
        !Number.isFinite(v.x) || !Number.isFinite(v.y) ||
        !oneOf(v.state, ['OUTSIDE', 'ENTERING', 'INSIDE', 'EXITING']) ||
        !oneOf(v.safety_state, [null, 'FALL', 'IMMOBILE']) ||
        (v.label !== undefined && typeof v.label !== 'string') ||
        Object.keys(v).some(k => !['id', 'x', 'y', 'state', 'safety_state', 'label'].includes(k)))
      throw new Error('Invalid passenger');
  }
  return text; // Keep original timestamp digits and all bytes; never JSON.stringify(p).
}
function sendBounded(socket, text, maxBufferedBytes, onError = () => {}) {
  if (!socket || socket.readyState !== 1) return 'offline';
  if (socket.bufferedAmount + Buffer.byteLength(text) > maxBufferedBytes) {
    socket.terminate(); // Discard queued old snapshots, rather than growing a replay backlog.
    return 'slow';
  }
  try {
    socket.send(text, { binary: false }, error => {
      if (error) { onError(); socket.terminate(); }
    });
    return 'sent';
  } catch {
    onError(); socket.terminate(); return 'error';
  }
}
function installSignals(stop) {
  const shutdown = () => { stop().catch(() => { process.exitCode = 1; }); };
  process.once('SIGINT', shutdown);
  process.once('SIGTERM', shutdown);
}
module.exports = { integer, limits, snapshotText, sendBounded, installSignals, now: () => performance.now() };
