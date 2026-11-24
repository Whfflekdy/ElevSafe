'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const { once } = require('node:events');
const { createRequire } = require('node:module');
const { execFileSync, spawn } = require('node:child_process');
const fs = require('node:fs');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const os = require('node:os');
const requireRelay = createRequire(require.resolve('../relay/package.json'));
const WebSocket = requireRelay('ws');
const { WebSocketServer } = WebSocket;
const { createRelay } = require('../relay/relay');
const { createBridge } = require('../bridge/bridge');
const { limits, snapshotText, sendBounded } = require('../relay/transport');
const { remainingControlMs } = require('../relay/control');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(predicate, label, ms = 2500) {
  const end = performance.now() + ms;
  while (!predicate()) {
    if (performance.now() >= end) throw new Error(`Timed out: ${label}`);
    await delay(10);
  }
}
function snapshot(us = 0) {
  return JSON.stringify({ timestamp: '2026-11-16T00:00:00Z', timestamp_us: us,
    door_state: 'UNKNOWN', door_command: null,
    passengers: [{ id: 23, x: 0.25, y: -0.8, state: 'OUTSIDE', safety_state: null, label: 'MOCK' }] });
}
// Log only endpoint/error fields, never URLs, credentials or payloads.
const fixtureCleanups = new WeakMap();
const serverStops = new WeakMap();
function diagnostic(t, role, stage, details = {}) {
  console.log(JSON.stringify({ fixture: role, test: t.name, pid: process.pid,
    node: process.version, platform: process.platform, time: new Date().toISOString(), stage, ...details }));
}
function errorFields(error) {
  return Object.fromEntries(['code', 'errno', 'syscall', 'address', 'port']
    .map(key => [key, error[key] ?? null]));
}
function registerCleanup(t, role, cleanup) {
  let entries = fixtureCleanups.get(t);
  if (!entries) {
    entries = [];
    fixtureCleanups.set(t, entries);
    t.after(async () => {
      const failures = [];
      // Reverse creation order; one failure must not skip other resources.
      for (const entry of [...entries].reverse()) {
        try { await entry(); } catch (error) { failures.push(error); }
      }
      if (failures.length) throw new AggregateError(failures, 'Fixture cleanup failed (see cleanup-failed diagnostics)');
    });
  }
  let stopping;
  const stop = () => {
    if (!stopping) stopping = (async () => {
      diagnostic(t, role, 'cleanup-start');
      try {
        await cleanup();
        diagnostic(t, role, 'cleanup-complete');
      } catch (error) {
        diagnostic(t, role, 'cleanup-failed', errorFields(error));
        throw error;
      }
    })();
    return stopping;
  };
  entries.push(stop);
  return stop;
}
async function readyFixture(t, role, requested, ready, address) {
  try {
    await ready;
    diagnostic(t, role, 'ready', { requested, actual: address() });
  } catch (error) {
    diagnostic(t, role, 'prepare-failed', { requested, ...errorFields(error) });
    throw error;
  }
}
async function startServer(t, role, port = 0) {
  const requested = { address: '127.0.0.1', port };
  diagnostic(t, role, 'prepare-start', { requested });
  let server;
  try { server = new WebSocketServer({ host: requested.address, port }); }
  catch (error) {
    diagnostic(t, role, 'create-failed', { requested, ...errorFields(error) });
    throw error;
  }
  serverStops.set(server, registerCleanup(t, role, async () => {
    for (const ws of server.clients) ws.terminate();
    await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  }));
  await readyFixture(t, role, requested, once(server, 'listening'), () => server.address());
  return server;
}
function managedRelay(t, options) {
  const role = 'relay', requested = { address: options.host, port: options.port };
  diagnostic(t, role, 'prepare-start', { requested });
  let relay;
  try { relay = createRelay(options); }
  catch (error) {
    diagnostic(t, role, 'create-failed', { requested, ...errorFields(error) });
    throw error;
  }
  registerCleanup(t, role, () => relay.stop());
  const ready = readyFixture(t, role, requested, relay.ready, () => relay.server.address());
  return { ...relay, ready };
}
function managedBridge(t, options) {
  const bridge = createBridge(options);
  registerCleanup(t, 'bridge', () => bridge.stop());
  return bridge;
}
function client(t, url, options) {
  const endpoint = new URL(url);
  const requested = { address: endpoint.hostname, port: Number(endpoint.port || (endpoint.protocol === 'wss:' ? 443 : 80)) };
  diagnostic(t, 'client', 'prepare-start', { requested });
  const ws = new WebSocket(url, options);
  ws.on('error', error => diagnostic(t, 'client', 'socket-error', errorFields(error)));
  registerCleanup(t, 'client', async () => {
    if (ws.readyState === WebSocket.CLOSED) return;
    const closed = new Promise(resolve => ws.once('close', resolve));
    ws.terminate();
    await closed;
  });
  ws.messages = [];
  ws.on('message', data => ws.messages.push(data.toString()));
  ws.once('open', () => diagnostic(t, 'client', 'ready', { requested }));
  return ws;
}
async function connect(t, url, options) {
  const ws = client(t, url, options);
  await once(ws, 'open');
  return ws;
}
async function rejectCode(t, url, expected, options) {
  const ws = client(t, url, options);
  const result = new Promise((resolve, reject) => {
    ws.once('unexpected-response', (req, response) => {
      response.resume();
      resolve(response.statusCode); // Settle before intentional termination emits error.
      ws.terminate();
    });
    ws.once('open', () => { reject(new Error('Unexpected successful connection')); ws.terminate(); });
    ws.once('error', reject);
    ws.once('close', () => reject(new Error('Connection closed before HTTP rejection')));
  });
  assert.equal(await result, expected);
}
async function startRelay(t, options = {}) {
  const key = randomBytes(16).toString('hex'); // Ephemeral test credential, never printed/persisted.
  const relay = managedRelay(t, { key, host: '127.0.0.1', port: 0, ...options });
  await relay.ready;
  const base = 'ws://127.0.0.1:' + relay.server.address().port;
  return { relay, key, viewerUrl: base + '/radar', ingestUrl: base + '/ingest?key=' + key, base };
}

test('configuration and payload validation preserve v7 bytes', () => {
  assert.throws(() => createBridge({ remoteUrl: 'http://invalid' }));
  assert.throws(() => createBridge({ remoteUrl: undefined }));
  assert.throws(() => createBridge({ remoteUrl: 'ws://127.0.0.1', localUrl: 'ws://user:pass@localhost' }));
  assert.throws(() => limits({ maxBufferedBytes: 1 }));
  assert.throws(() => createRelay({ key: '', port: 0 }));
  const text = snapshot(9007199254740991).replace('9007199254740991', '9007199254740993');
  assert.equal(snapshotText(Buffer.from(text), false, 65536), text);
  // Match Backend isSnapshot's existing v7 fields; preserve original text.
  for (const door of ['UNKNOWN', 'OPEN', 'CLOSED', 'OPENING', 'CLOSING']) {
    for (const command of [null, 'OPEN', 'HOLD', 'CLOSE']) {
      const value = JSON.parse(snapshot());
      value.door_state = door; value.door_command = command; value.passengers = [];
      const original = JSON.stringify(value, null, 2);
      assert.equal(snapshotText(Buffer.from(original), false, 65536), original);
    }
  }
  for (const id of [23, 'track-23']) {
    for (const state of ['OUTSIDE', 'ENTERING', 'INSIDE', 'EXITING']) {
      for (const safety of [null, 'FALL', 'IMMOBILE']) {
        const value = JSON.parse(snapshot());
        Object.assign(value.passengers[0], { id, state, safety_state: safety, x: -1.25, y: 0 });
        delete value.passengers[0].label;
        const original = JSON.stringify(value);
        assert.equal(snapshotText(Buffer.from(original), false, 65536), original);
      }
    }
  }
  const mutations = [
    p => { delete p.timestamp; }, p => { p.timestamp = 123; },
    p => { p.timestamp_us = -1; }, p => { p.timestamp_us = 0.5; },
    p => { p.door_state = 'INVALID'; }, p => { delete p.door_command; },
    p => { p.door_command = 'AUTO'; }, p => { p.extra = true; },
    p => { p.passengers = {}; }, p => { p.passengers[0].id = {}; },
    p => { p.passengers[0].x = '0'; }, p => { p.passengers[0].y = null; },
    p => { p.passengers[0].state = 'INVALID'; },
    p => { p.passengers[0].safety_state = 'INVALID'; },
    p => { p.passengers[0].label = null; }, p => { p.passengers[0].extra = 1; }
  ];
  for (const mutate of mutations) {
    const value = JSON.parse(snapshot()); mutate(value);
    assert.throws(() => snapshotText(Buffer.from(JSON.stringify(value)), false, 65536));
  }
  for (const number of ['1e400', '-1e400', 'NaN', 'Infinity']) {
    const invalid = snapshot().replace('"x":0.25', '"x":' + number);
    assert.throws(() => snapshotText(Buffer.from(invalid), false, 65536));
  }
  assert.throws(() => snapshotText(Buffer.from('not-json'), false, 65536));
  assert.throws(() => snapshotText(Buffer.from('[]'), false, 65536));
  assert.throws(() => snapshotText(Buffer.from(snapshot()), true, 65536));
  assert.throws(() => snapshotText(Buffer.from(snapshot()), false, 1));
});

test('bounded sender uses projected bytes, drops slow peers and handles send errors', () => {
  let terminated = 0, sent = 0, errors = 0;
  const peer = { readyState: 1, bufferedAmount: 127,
    terminate() { ++terminated; }, send(text, opts, cb) { ++sent; cb(); } };
  assert.equal(sendBounded(peer, 'a', 128), 'sent');
  assert.equal(sendBounded(peer, 'ab', 128), 'slow');
  assert.equal(sent, 1); assert.equal(terminated, 1);
  peer.bufferedAmount = 0;
  peer.send = () => { throw new Error('test'); };
  assert.equal(sendBounded(peer, 'a', 128, () => ++errors), 'error');
  peer.send = (text, options, cb) => cb(new Error('test'));
  sendBounded(peer, 'a', 128, () => ++errors);
  assert.equal(errors, 2);
  assert.equal(sendBounded(null, 'a', 128), 'offline');
});

test('single ingest, identical viewers, commands blocked, invalid input and disconnect cache', { timeout: 10000 }, async t => {
  const r = await startRelay(t);
  await rejectCode(t, r.base + '/control', 404);
  await rejectCode(t, r.base + '/control-link', 404);
  await rejectCode(t, r.base + '/ingest?key=invalid-test', 403);
  const producer = await connect(t, r.ingestUrl);
  await rejectCode(t, r.ingestUrl, 409);
  const a = await connect(t, r.viewerUrl), b = await connect(t, r.viewerUrl);
  producer.send(snapshot(100));
  await until(() => a.messages.length === 1 && b.messages.length === 1, 'both viewers');
  assert.equal(a.messages[0], snapshot(100)); assert.deepEqual(a.messages, b.messages);
  a.send('{"door_command":"OPEN"}'); a.send('{"door_command":null}');
  await until(() => r.relay.stats.ignoredCommands === 2, 'read-only commands');
  assert.equal(producer.messages.length, 0);
  for (const text of ['not-json', '{}', '{"door_command":"CLOSE"}']) producer.send(text);
  producer.send(Buffer.from(snapshot()));
  await until(() => r.relay.stats.invalid === 4, 'invalid payloads rejected');
  assert.equal(r.relay.stats.accepted, 1);
  const c = await connect(t, r.viewerUrl);
  await until(() => c.messages.length === 1, 'fresh cached snapshot');
  producer.close(); await once(producer, 'close');
  await until(() => !r.relay.cached(), 'disconnect cache invalidated');
  const d = await connect(t, r.viewerUrl);
  await delay(80); assert.equal(d.messages.length, 0);
  const next = await connect(t, r.ingestUrl);
  next.send(snapshot(0)); // A new Radar session may restart relative time at zero.
  await until(() => d.messages.length === 1, 'new session');
  assert.equal(d.messages[0], snapshot(0));
});

test('monotonic stale expiration; invalid traffic cannot refresh cache', { timeout: 5000 }, async t => {
  const r = await startRelay(t, { staleMs: 120 });
  const p = await connect(t, r.ingestUrl);
  p.send(snapshot(999999999));
  await until(() => r.relay.stats.accepted === 1, 'accepted');
  await delay(80); p.send('bad');
  await until(() => !r.relay.cached(), 'stale expiration');
  const v = await connect(t, r.viewerUrl);
  await delay(80); assert.equal(v.messages.length, 0);
  p.send(snapshot(0));
  await until(() => v.messages.length === 1, 'fresh after stale');
});

test('oversized input closes only offending connection; relay stays live', { timeout: 5000 }, async t => {
  const r = await startRelay(t, { maxMessageBytes: 1024, maxBufferedBytes: 2048 });
  const p = await connect(t, r.ingestUrl);
  p.send('x'.repeat(1025)); await once(p, 'close');
  await until(() => r.relay.stats.errors > 0, 'payload error');
  const p2 = await connect(t, r.ingestUrl);
  const v = await connect(t, r.viewerUrl);
  p2.send(snapshot()); await until(() => v.messages.length === 1, 'relay survived');
});

test('Fake Backend -> Bridge -> Relay, restart recovery and shutdown timers', { timeout: 15000 }, async t => {
  let fake, relay, bridge;
  fake = await startServer(t, 'fake-backend');
  const fakePort = fake.address().port;
  let fakeCommands = 0;
  fake.on('connection', ws => { ws.on('error', () => {}); ws.on('message', () => ++fakeCommands); });
  const key = randomBytes(16).toString('hex');
  relay = managedRelay(t, { key, host: '127.0.0.1', port: 0 }); await relay.ready;
  const relayPort = relay.server.address().port;
  bridge = managedBridge(t, { localUrl: `ws://127.0.0.1:${fakePort}/radar`,
    remoteUrl: `ws://127.0.0.1:${relayPort}/ingest?key=${key}`, reconnectMs: 50, connectTimeoutMs: 500 });
  await until(() => bridge.stats.localOpens === 1 && bridge.stats.remoteOpens === 1, 'initial bridge');
  const v = await connect(t, `ws://127.0.0.1:${relayPort}/radar`);
  for (const c of fake.clients) c.send(snapshot(50));
  await until(() => v.messages.length === 1, 'end-to-end');
  v.send('{"door_command":"CLOSE"}');
  await until(() => relay.stats.ignoredCommands === 1, 'ignored command'); assert.equal(fakeCommands, 0);
  await relay.stop();
  for (const c of fake.clients) c.send(snapshot(100));
  await until(() => bridge.stats.dropped >= 1, 'offline drops');
  relay = managedRelay(t, { key, host: '127.0.0.1', port: relayPort }); await relay.ready;
  await until(() => bridge.stats.remoteOpens === 2, 'relay restart');
  const v2 = await connect(t, `ws://127.0.0.1:${relayPort}/radar`);
  await delay(80); assert.equal(v2.messages.length, 0); // No replay from before reconnect.
  await serverStops.get(fake)();
  fake = await startServer(t, 'fake-backend', fakePort);
  fake.on('connection', ws => ws.on('error', () => {}));
  await until(() => bridge.stats.localOpens === 2, 'Backend restart');
  for (const c of fake.clients) c.send(snapshot(0));
  await until(() => v2.messages.length === 1, 'recovered data');
  assert.equal(v2.messages[0], snapshot(0));
  await bridge.stop(); await until(() => !relay.cached(), 'Bridge stop invalidates relay');
  const opens = bridge.stats.localOpens + bridge.stats.remoteOpens;
  await delay(160); assert.equal(bridge.stats.localOpens + bridge.stats.remoteOpens, opens);
  assert.equal(fake.clients.size, 0);
});

test('real paused viewer is bounded while healthy viewer continues', { timeout: 15000 }, async t => {
  const r = await startRelay(t);
  const p = await connect(t, r.ingestUrl);
  const slow = await connect(t, r.viewerUrl), healthy = await connect(t, r.viewerUrl);
  slow._socket.pause(); // Test-only network backpressure, not product code.
  const value = JSON.parse(snapshot()); value.passengers[0].label = 'x'.repeat(60000);
  for (let i = 0; i < 600 && r.relay.stats.slowViewers === 0; ++i) {
    value.timestamp_us = i;
    p.send(JSON.stringify(value));
    await until(() => healthy.messages.length > i, 'healthy viewer makes progress');
  }
  assert.equal(r.relay.stats.slowViewers, 1);
  assert.equal(healthy.readyState, WebSocket.OPEN);
  assert.ok(healthy.messages.length > 0);
  slow._socket.resume();
});

test('real slow remote is dropped without disconnecting local Backend', { timeout: 15000 }, async t => {
  let fake, remote, bridge;
  fake = await startServer(t, 'fake-backend');
  remote = await startServer(t, 'slow-remote');
  remote.on('connection', ws => { ws.on('error', () => {}); ws._socket.pause(); });
  bridge = managedBridge(t, { localUrl: `ws://127.0.0.1:${fake.address().port}/radar`,
    remoteUrl: `ws://127.0.0.1:${remote.address().port}/ingest`, reconnectMs: 2000 });
  await until(() => bridge.stats.localOpens === 1 && bridge.stats.remoteOpens === 1, 'connected');
  const value = JSON.parse(snapshot()); value.passengers[0].label = 'x'.repeat(60000);
  for (let i = 0; i < 600 && bridge.stats.slowRemote === 0; ++i) {
    for (const ws of fake.clients) ws.send(JSON.stringify(value));
    await until(() => bridge.stats.forwarded + bridge.stats.dropped > i, 'local messages consumed');
  }
  assert.equal(bridge.stats.slowRemote, 1);
  assert.equal(bridge.stats.localOpens, 1);
  assert.equal(fake.clients.size, 1);
});

test('heartbeat terminates unresponsive ingest and invalidates cache', { timeout: 5000 }, async t => {
  const r = await startRelay(t, { heartbeatMs: 80 });
  const p = await connect(t, r.ingestUrl, { autoPong: false });
  p.send(snapshot()); await until(() => r.relay.cached(), 'cache filled');
  await until(() => p.readyState === WebSocket.CLOSED, 'heartbeat close');
  assert.equal(r.relay.cached(), false);
});

test('listen error is reported and cleanup releases timers', { timeout: 5000 }, async t => {
  const r = await startRelay(t);
  const rejected = managedRelay(t, { key: randomBytes(16).toString('hex'), host: '127.0.0.1', port: r.relay.server.address().port });
  await assert.rejects(rejected.ready, { code: 'EADDRINUSE' });
  await rejected.stop();
  assert.equal(rejected.stats.errors, 1);
  assert.throws(() => createBridge({ localUrl: '', remoteUrl: 'ws://127.0.0.1/ingest' }));
});

// The certificate and all credentials exist only in this test's temporary directory/process.
function controlTls(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'elevsafe-control-'));
  const key = path.join(dir, 'key.pem'), cert = path.join(dir, 'cert.pem');
  t.after(() => {
    fs.rmSync(key, { force: true });
    fs.rmSync(cert, { force: true });
    fs.rmdirSync(dir);
  });
  execFileSync(process.env.ELEVSAFE_TEST_OPENSSL || 'openssl',
    ['req', '-x509', '-nodes', '-newkey', 'rsa:2048', '-days', '1', '-subj', '/CN=127.0.0.1',
      '-addext', 'subjectAltName=IP:127.0.0.1', '-keyout', key, '-out', cert], { stdio: 'ignore' });
  return { key: fs.readFileSync(key), cert: fs.readFileSync(cert) };
}

function controlHttp(url, ca, headers) {
  return new Promise((resolve, reject) => {
    const request = https.request(url, { ca, method: 'POST', headers }, response => {
      response.resume();
      response.once('end', () => resolve(response));
    });
    request.once('error', reject);
    request.end();
  });
}

async function controlRelayFixture(t, options = {}) {
  const tls = controlTls(t);
  const token = () => randomBytes(32).toString('hex');
  const operatorToken = token(), bridgeToken = token(), localToken = token(), key = token();
  // Bind on port 0 while using a fixed test Origin. Origin is checked independently of routing.
  const origin = 'https://127.0.0.1';
  const start = (port = 0) => managedRelay(t, { key, host: '127.0.0.1', port, tls, ...options,
    control: { operatorToken, bridgeToken, origin, ...options.control } });
  const relay = start();
  await relay.ready;
  const host = '127.0.0.1:' + relay.server.address().port;
  const browser = async () => {
    const response = await controlHttp('https://' + host + '/control/login', tls.cert,
      { Origin: origin, Authorization: 'Bearer ' + operatorToken });
    assert.equal(response.statusCode, 204);
    const cookie = response.headers['set-cookie'][0];
    assert.match(cookie, /Secure; HttpOnly; SameSite=Strict/);
    const headers = { Origin: origin, Cookie: cookie.split(';')[0] };
    return { ws: await connect(t, 'wss://' + host + '/control', { ca: tls.cert, headers }), headers };
  };
  const restart = async () => {
    const replacement = start(Number(host.split(':')[1]));
    await replacement.ready;
    return replacement;
  };
  return { relay, host, origin, browser, restart, key, operatorToken, bridgeToken, localToken, tls };
}

function controlResults(ws) {
  return ws.messages.map(line => JSON.parse(line));
}

function expectControl(ws, predicate, label) {
  return until(() => controlResults(ws).some(predicate), label, 4500).then(() => controlResults(ws).find(predicate));
}

function operatorRequest(ws, id, command) {
  ws.send(JSON.stringify({ type: 'door_request', client_request_id: id, door_command: command }));
}

test('Relay TTL budget is bounded by monotonic time and UTC expiry', () => {
  assert.equal(remainingControlMs(2000, 12000, 400, 10400), 1600);
  assert.equal(remainingControlMs(2000, 12000, 2000, 12000), 0);
  assert.equal(remainingControlMs(2000, 12000, 2100, 8000), 0);
  assert.equal(remainingControlMs(2000, 12000, 400, 12001), 0);
  assert.equal(remainingControlMs(2000, 12000, 400.5, 10400), 1599);
});

test('WSS control authenticates separately, fences requests and keeps radar read-only', { timeout: 15000 }, async t => {
  const r = await controlRelayFixture(t, { control: { requestsPerSecond: 5, ttlMs: 1000 } });
  const { host, tls, origin } = r;
  const base = 'wss://' + host;
  await rejectCode(t, base + '/control', 403, { ca: tls.cert, headers: { Origin: origin } });
  await rejectCode(t, base + '/control-link', 403, { ca: tls.cert, headers: { Authorization: 'Bearer ' + r.key } });
  const { ws: user, headers } = await r.browser();
  await rejectCode(t, base + '/control', 403, { ca: tls.cert, headers: { ...headers, Origin: 'https://different.invalid' } });
  const link = await connect(t, base + '/control-link',
    { ca: tls.cert, headers: { Authorization: 'Bearer ' + r.bridgeToken } });
  const binding = await expectControl(link, m => m.type === 'control_link', 'link generation');
  const viewer = await connect(t, base + '/radar', { ca: tls.cert });
  const second = await connect(t, base + '/radar', { ca: tls.cert });
  const producer = await connect(t, base + '/ingest?key=' + r.key, { ca: tls.cert });
  producer.send(snapshot(30));
  await until(() => viewer.messages.length === 1 && second.messages.length === 1, 'viewer snapshots');
  assert.equal(viewer.messages[0], snapshot(30));
  assert.deepEqual(second.messages, viewer.messages);
  viewer.send('{"door_command":"CLOSE"}');
  await until(() => r.relay.stats.ignoredCommands === 1, 'viewer read-only');
  assert.equal(controlResults(link).filter(m => m.type === 'door_request').length, 0);

  const backend = 'backend-one';
  link.send(JSON.stringify({ type: 'control_ready', enabled: true, backend_connected: true, ready: true, reason: null, backend_session_id: backend }));
  await expectControl(user, m => m.type === 'control_ready' && m.ready, 'control ready');
  user.send('{"type":"door_request","client_request_id":"bad","door_command":null}');
  await expectControl(user, m => m.reason === 'INVALID_COMMAND', 'invalid command');
  operatorRequest(user, 'first', 'OPEN');
  const accepted = await expectControl(user, m => m.client_request_id === 'first' && m.status === 'ACCEPTED', 'relay acceptance');
  const request = await expectControl(link, m => m.type === 'door_request', 'relay forwarding');
  assert.equal(request.request_id, accepted.request_id);
  assert.equal(request.backend_session_id, backend);
  assert.equal(request.control_session_id, binding.control_session_id);
  assert.equal(Date.parse(request.expires_at) - Date.parse(request.issued_at), 1000);
  link.send(JSON.stringify({ type: 'control_validate', request_id: request.request_id,
    control_session_id: request.control_session_id, backend_session_id: request.backend_session_id }));
  const proof = await expectControl(link, m => m.type === 'control_validity', 'Relay expiry budget');
  assert.equal(proof.request_id, request.request_id);
  assert.ok(proof.remaining_ms > 0 && proof.remaining_ms <= 1000);
  operatorRequest(user, 'first', 'OPEN');
  await expectControl(user, m => m.reason === 'DUPLICATE_REQUEST', 'duplicate blocked');
  operatorRequest(user, 'second', 'CLOSE');
  await expectControl(user, m => m.reason === 'QUEUE_FULL', 'one pending');
  operatorRequest(user, 'third', 'CLOSE');
  await expectControl(user, m => m.client_request_id === 'third', 'fifth attempt');
  operatorRequest(user, 'fourth', 'CLOSE');
  await expectControl(user, m => m.reason === 'RATE_LIMIT', 'rate limit');
  assert.equal(controlResults(link).filter(m => m.type === 'door_request').length, 1);

  const response = await controlHttp('https://' + host + '/control/logout', tls.cert, headers);
  assert.equal(response.statusCode, 204);
  await expectControl(link, m => m.type === 'control_revoke' &&
    m.operator_id === request.operator_id && m.control_session_id === request.control_session_id, 'logout revocation');
  await rejectCode(t, base + '/control', 403, { ca: tls.cert, headers });
  producer.send(snapshot(40));
  await until(() => viewer.messages.at(-1) === snapshot(40) && second.messages.at(-1) === snapshot(40), 'snapshot after control revocation');
  assert.deepEqual(second.messages, viewer.messages);
});

if (process.env.ELEVSAFE_TEST_BACKEND) {
  test('WSS Dashboard protocol through Relay and Bridge applies real virtual-door commands', { timeout: 22000 }, async t => {
    const probe = await startServer(t, 'backend-port');
    const port = probe.address().port;
    await serverStops.get(probe)();
    const r = await controlRelayFixture(t);
    const backendUrl = 'ws://127.0.0.1:' + port;
    const child = spawn(process.env.ELEVSAFE_TEST_BACKEND,
      ['--ws-mock', '--ws-door-sim', '--ws-duration', '15', '--ws-port', String(port), '--ws-remote-control'],
      { env: { ...process.env, CONTROL_LOCAL_TOKEN: r.localToken }, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    const lines = [];
    let stderr = '';
    const exited = new Promise(resolve => child.once('exit', resolve));
    child.once('error', error => lines.push('spawn-error ' + error.code));
    child.stdout.resume();
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', text => {
      stderr += text;
      const parts = stderr.split('\n');
      stderr = parts.pop();
      lines.push(...parts);
    });
    registerCleanup(t, 'backend', async () => {
      if (child.exitCode !== null || child.signalCode !== null || !child.pid) return;
      child.kill();
      await exited;
    });
    await until(() => lines.some(line => line.includes('[ws] listening ')), 'real Backend listener', 5000);
    await rejectCode(t, backendUrl + '/control-local', 403);
    const local = await connect(t, backendUrl + '/radar');
    const fakeRemoteClaim = '{"door_command":"OPEN","source":"remote"}';
    local.send(fakeRemoteClaim);
    await until(() => lines.some(line => line.includes('invalid manual request')), 'source spoof rejected');
    const via = 'wss://' + r.host;
    const viewer = await connect(t, via + '/radar', { ca: r.tls.cert });
    managedBridge(t, { localUrl: backendUrl + '/radar', remoteUrl: via + '/ingest?key=' + r.key,
      reconnectMs: 80, ca: r.tls.cert, control: { remoteUrl: via + '/control-link', localUrl: backendUrl + '/control-local',
        bridgeToken: r.bridgeToken, localToken: r.localToken, ca: r.tls.cert },
    });
    const { ws: operator } = await r.browser();
    await expectControl(operator, m => m.type === 'control_ready' && m.ready, 'real Backend control ready');
    const started = performance.now();
    operatorRequest(operator, 'open', 'OPEN');
    await expectControl(operator, m => m.client_request_id === 'open' && m.status === 'ACCEPTED', 'OPEN accepted');
    await expectControl(operator, m => m.client_request_id === 'open' && m.status === 'APPLIED', 'OPEN applied');
    console.log('control OPEN request-to-result ms=' + Math.round(performance.now() - started));
    await expectControl(viewer, m => m.door_state === 'OPENING' && m.door_command === 'OPEN', 'opening published');
    local.send('{"door_command":"CLOSE"}');
    await until(() => lines.some(line => line.includes('remote priority')), 'lower-priority local request discarded');
    await expectControl(viewer, m => m.door_state === 'OPEN', 'open completed');
    operatorRequest(operator, 'hold', 'HOLD');
    await expectControl(operator, m => m.client_request_id === 'hold' && m.status === 'APPLIED', 'HOLD applied');
    operator.close();
    await once(operator, 'close');
    const before = viewer.messages.length;
    const holdStarted = JSON.parse(viewer.messages.at(-1)).timestamp_us;
    await until(() => viewer.messages.length > before &&
      JSON.parse(viewer.messages.at(-1)).timestamp_us >= holdStarted + 4100000,
    'remote HOLD remains past the normal close deadline', 5200);
    assert.ok(viewer.messages.slice(before).every(line => JSON.parse(line).door_state === 'OPEN'));
    local.send('{"door_command":"CLOSE"}');
    const localClosing = await expectControl(local, m => m.door_state === 'CLOSING' &&
      m.door_command === 'CLOSE' && m.timestamp_us > holdStarted, 'local CLOSE after operator disconnect');
    const localClosed = await expectControl(local, m => m.door_state === 'CLOSED' &&
      m.timestamp_us > localClosing.timestamp_us, 'local close completed');
    const { ws: second } = await r.browser();
    await expectControl(second, m => m.type === 'control_ready' && m.ready, 'fresh control connection');
    operatorRequest(second, 'fresh-open', 'OPEN');
    await expectControl(second, m => m.client_request_id === 'fresh-open' && m.status === 'APPLIED', 'fresh OPEN applied');
    await expectControl(viewer, m => m.door_state === 'OPENING' && m.door_command === 'OPEN' &&
      m.timestamp_us > localClosed.timestamp_us, 'fresh opening published');
    assert.equal(lines.filter(line => line.includes('source=remote') && line.includes('decision=applied')).length, 3);
    await exited;
    assert.equal(child.exitCode, 0, 'Backend exited normally');
  });
}


function loggedChild(t, role, executable, args, extraEnv = {}) {
  const proc = spawn(executable, args, { windowsHide: true,
    env: { ...process.env, ...extraEnv }, stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  for (const stream of [proc.stdout, proc.stderr]) {
    stream.setEncoding('utf8');
    stream.on('data', text => { output += text; });
  }
  const finished = new Promise(resolve => proc.once('exit', (code, signal) => resolve({ code, signal })));
  proc.on('error', error => { output += 'spawn-error ' + error.code; });
  registerCleanup(t, role, async () => {
    if (proc.pid && proc.exitCode === null && proc.signalCode === null) {
      proc.kill();
      await finished;
    }
  });
  return { output: () => output, finished };
}

if (process.env.ELEVSAFE_TEST_BACKEND) {
  test('WSS HOLD takeover survives Relay and Bridge loss while local CLOSE recovers', { timeout: 36000 }, async t => {
    const probe = await startServer(t, 'takeover-backend-port');
    const port = probe.address().port;
    await serverStops.get(probe)();
    const ttlMs = 750;
    const r = await controlRelayFixture(t, { control: { ttlMs } });
    const backend = loggedChild(t, 'takeover-backend', process.env.ELEVSAFE_TEST_BACKEND,
      ['--ws-mock', '--ws-door-sim', '--ws-duration', '32', '--ws-port', String(port), '--ws-remote-control'],
      { CONTROL_LOCAL_TOKEN: r.localToken, CONTROL_TTL_MS: String(ttlMs) });
    await until(() => backend.output().includes('[ws] listening '), 'Backend startup', 5000);
    const localRoot = 'ws://127.0.0.1:' + port, via = 'wss://' + r.host;
    const local = await connect(t, localRoot + '/radar');
    let link;
    const captureLink = relay => relay.server.on('upgrade', (req, socket) => {
      if (req.url === '/control-link') link = socket;
    });
    captureLink(r.relay);
    managedBridge(t, { localUrl: localRoot + '/radar', remoteUrl: via + '/ingest?key=' + r.key,
      reconnectMs: 300, ca: r.tls.cert, control: { remoteUrl: via + '/control-link',
        localUrl: localRoot + '/control-local', bridgeToken: r.bridgeToken,
        localToken: r.localToken, ca: r.tls.cert, ttlMs } });

    async function openOperator() {
      const { ws } = await r.browser();
      await expectControl(ws, m => m.type === 'control_ready' && m.ready, 'control ready');
      return ws;
    }
    const applied = (ws, id) => expectControl(ws, m => m.client_request_id === id && m.status === 'APPLIED', id);
    const localDoor = (index, state, command) => until(() => controlResults(local).slice(index)
      .some(m => m.door_state === state && (command === undefined || m.door_command === command)), state, 5200);
    const fingerprint = () => backend.output().split('\n')
      .filter(line => line.includes('decision=applied') && line.includes('source=remote')).length;

    async function heldThenLocalClose(disconnect, label) {
      await disconnect();
      const from = local.messages.length;
      const first = controlResults(local).at(-1).timestamp_us;
      const count = fingerprint();
      await until(() => controlResults(local).at(-1).timestamp_us >= first + 4100000, label + ' HOLD remains', 5200);
      assert.ok(controlResults(local).slice(from).every(m => m.door_state === 'OPEN' && m.door_command === null));
      assert.equal(fingerprint(), count, label + ' never replays a remote event');
      const start = local.messages.length;
      local.send('{"door_command":"CLOSE"}');
      await localDoor(start, 'CLOSING', 'CLOSE');
      await localDoor(start, 'CLOSED', null);
      console.log(label + ': held OPEN without automatic CLOSE, new local CLOSE applied');
    }

    let operator = await openOperator();
    let start = local.messages.length;
    local.send('{"door_command":"HOLD"}');
    await localDoor(start, 'OPEN');
    start = local.messages.length;
    operatorRequest(operator, 'takeover', 'HOLD');
    await applied(operator, 'takeover');
    await localDoor(start, 'OPEN', 'HOLD');
    operatorRequest(operator, 'redundant', 'HOLD');
    await expectControl(operator, m => m.client_request_id === 'redundant' && m.status === 'REJECTED' &&
      m.reason === 'ALREADY_IN_STATE', 'same owner and command rejected without an event');
    operatorRequest(operator, 'takeover', 'HOLD');
    await expectControl(operator, m => m.client_request_id === 'takeover' && m.reason === 'DUPLICATE_REQUEST',
      'duplicate request ID rejected');
    local.send('{"door_command":"HOLD"}');
    local.send('{"door_command":"CLOSE"}');
    await until(() => (backend.output().match(/remote priority/g) || []).length >= 2, 'lower priorities blocked');
    assert.equal(fingerprint(), 1);
    assert.ok(link);

    // Pause reads on the authenticated Relay link. A cannot obtain validity before TTL.
    link.pause();
    try {
      operatorRequest(operator, 'expired-close', 'CLOSE');
      await expectControl(operator, m => m.client_request_id === 'expired-close' && m.status === 'ACCEPTED',
        'Relay admitted CLOSE before pause released');
      await delay(ttlMs + 80);
    } finally {
      link.resume();
    }
    await expectControl(operator, m => m.client_request_id === 'expired-close' && m.status === 'EXPIRED',
      'CLOSE expired during cross-link validity check');
    assert.equal(fingerprint(), 1, 'expired CLOSE never reached application');
    assert.equal(controlResults(local).at(-1).door_state, 'OPEN');

    await heldThenLocalClose(() => r.relay.stop(), 'Relay stop');
    captureLink(await r.restart());
    operator = await openOperator();
    start = local.messages.length;
    operatorRequest(operator, 'bridge-hold', 'HOLD');
    await applied(operator, 'bridge-hold');
    await localDoor(start, 'OPEN');
    const viewer = await connect(t, via + '/radar', { ca: r.tls.cert });
    const previousLink = link;
    await heldThenLocalClose(async () => {
      const since = operator.messages.length;
      previousLink.destroy();
      await until(() => controlResults(operator).slice(since).some(m => m.type === 'control_ready' && !m.ready),
        'Bridge control path ended');
    }, 'Bridge control link loss');
    assert.equal(viewer.readyState, WebSocket.OPEN);
    await until(() => link !== previousLink && controlResults(operator).at(-1)?.ready === true, 'control link recovered');
    start = local.messages.length;
    operatorRequest(operator, 'fresh-after-link', 'OPEN');
    await applied(operator, 'fresh-after-link');
    await localDoor(start, 'OPENING', 'OPEN');
    assert.equal(fingerprint(), 3);
    console.log('HOLD takeover emits one event, expired CLOSE not applied, fresh remote command applied after reconnect');
  });
}

if (process.env.ELEVSAFE_TEST_BACKEND && process.env.ELEVSAFE_TEST_FOV_EMULATOR) {
  test('FOV_t03 WSS control drops stale requests and fences the next Radar session', { timeout: 30000 }, async t => {
    assert.ok(process.env.ELEVSAFE_TEST_FOV_DATA && process.env.ELEVSAFE_TEST_FOV_CONFIG);
    const emulator = loggedChild(t, 'emulator', process.env.ELEVSAFE_TEST_FOV_EMULATOR,
      [process.env.ELEVSAFE_TEST_FOV_DATA]);
    await until(() => emulator.output().includes('READY record=FOV_t03'), 'headless FOV ready', 6000);
    const probe = await startServer(t, 'fov-backend-port');
    const port = probe.address().port;
    await serverStops.get(probe)();
    const r = await controlRelayFixture(t);
    const backend = loggedChild(t, 'fov-backend', process.env.ELEVSAFE_TEST_BACKEND,
      ['127.0.0.1', '29172', '--signal-config', process.env.ELEVSAFE_TEST_FOV_CONFIG,
        '--websocket', '--ws-port', String(port), '--emulator-door-sim', '--ws-remote-control',
        '--ws-periodic-snapshot', '--radar-reconnect', '--radar-idle-timeout-ms', '1800'],
      { CONTROL_LOCAL_TOKEN: r.localToken });
    await until(() => backend.output().includes('[ws] listening '), 'FOV Backend ready', 5000);
    const via = 'wss://' + r.host;
    const local = 'ws://127.0.0.1:' + port;
    const viewer = await connect(t, via + '/radar', { ca: r.tls.cert });
    const { ws: operator } = await r.browser();
    managedBridge(t, { localUrl: local + '/radar', remoteUrl: via + '/ingest?key=' + r.key,
      reconnectMs: 80, ca: r.tls.cert, control: { remoteUrl: via + '/control-link',
        localUrl: local + '/control-local', bridgeToken: r.bridgeToken, localToken: r.localToken, ca: r.tls.cert } });
    const first = await expectControl(operator, m => m.type === 'control_ready' && m.ready, 'first Radar epoch');
    operatorRequest(operator, 'fov-hold', 'HOLD');
    await expectControl(operator, m => m.client_request_id === 'fov-hold' && m.status === 'APPLIED', 'HOLD applied to FOV');
    await expectControl(viewer, m => m.door_command === 'HOLD', 'HOLD was published');
    await until(() => controlResults(operator).some(m => m.reason === 'RADAR_STALE'), 'stale observation closes admission', 7000);
    operatorRequest(operator, 'stale-close', 'CLOSE');
    await expectControl(operator, m => m.client_request_id === 'stale-close' && m.status === 'REJECTED', 'stale CLOSE rejected');
    await until(() => controlResults(operator).some(m => m.type === 'control_ready' && m.ready &&
      m.backend_session_id !== first.backend_session_id), 'second Radar epoch ready', 8000);
    await until(() => controlResults(viewer).some((m, i, all) => i > 0 && m.timestamp_us < all[i - 1].timestamp_us),
      'first Snapshot of new Radar session', 4500);
    const snapshots = controlResults(viewer);
    const newSession = snapshots.findIndex((m, i) => i > 0 && m.timestamp_us < snapshots[i - 1].timestamp_us);
    assert.equal(snapshots[newSession].door_state, 'CLOSED');
    assert.equal(snapshots[newSession].door_command, null);
    assert.ok(snapshots.slice(newSession).every(m => m.door_command !== 'HOLD'), 'previous HOLD was replayed');
    console.log('FOV new Radar session starts CLOSED, observation timestamp_us=' + snapshots[newSession].timestamp_us);
    operatorRequest(operator, 'resumed-open', 'OPEN');
    await expectControl(operator, m => m.client_request_id === 'resumed-open' && m.status === 'APPLIED', 'new session applied');
    await expectControl(viewer, m => m.door_state === 'OPENING' && m.door_command === 'OPEN', 'current session OPEN was published');
    assert.ok(controlResults(viewer).every(m => Object.keys(m).length === 5), 'Snapshot JSON v7 changed');
    assert.ok(controlResults(viewer).some((m, i, all) => i > 0 && m.timestamp_us < all[i - 1].timestamp_us),
      'same viewer never saw the new session clock');
    assert.equal((await emulator.finished).code, 0);
    assert.equal((await backend.finished).code, 0);
    const log = backend.output();
    assert.match(log, /background=enabled/);
    assert.match(log, /session=1 end stage=idle_timeout/);
    assert.match(log, /session=2 end stage=eof/);
    assert.match(log, /tracking-demo.*frames=[1-9]/);
    assert.doesNotMatch(log, /command=CLOSE.*decision=applied.*source=remote/);
    console.log('FOV control: old session HOLD, stale CLOSE rejected, new session OPEN, snapshots=' + viewer.messages.length);
  });
}

if (process.env.ELEVSAFE_TEST_BACKEND) {
  test('remote CLOSE overrides live C HOLD and blocks local reversal until completion', { timeout: 18000 }, async t => {
    const tcp = net.createServer();
    const radarConnection = once(tcp, 'connection');
    let peer;
    registerCleanup(t, 'synthetic-radar', async () => {
      peer?.destroy();
      if (tcp.listening) await new Promise(resolve => tcp.close(resolve));
    });
    tcp.listen(0, '127.0.0.1');
    await once(tcp, 'listening');
    const probe = await startServer(t, 'arbitration-backend-port');
    const port = probe.address().port;
    await serverStops.get(probe)();
    const r = await controlRelayFixture(t);
    const cfgDir = fs.mkdtempSync(path.join(os.tmpdir(), 'elevsafe-arbitration-'));
    const config = path.join(cfgDir, 'signal.cfg');
    t.after(() => { fs.rmSync(config, { force: true }); fs.rmdirSync(cfgDir); });
    let content = fs.readFileSync(path.join(__dirname, '../../backend/tests/data/signal_integration.cfg'), 'utf8');
    for (const [key, value] of Object.entries({
      'x_min': 0, 'x_max': 1, 'y_min': -0.2, 'y_max': 0.2, 'z_min': 0, 'z_max': 2,
    })) content = content.replace('door.' + key + ' = ' + (key.endsWith('min') ? 100 : 101), 'door.' + key + ' = ' + value);
    fs.writeFileSync(config, content);
    const backend = loggedChild(t, 'arbitration-backend', process.env.ELEVSAFE_TEST_BACKEND,
      ['127.0.0.1', String(tcp.address().port), '--signal-config', config, '--websocket', '--ws-port', String(port),
        '--emulator-door-sim', '--emulator-auto-cycle', '--ws-remote-control'],
      { CONTROL_LOCAL_TOKEN: r.localToken });
    await until(() => backend.output().includes('[ws] listening '), 'arbitration Backend ready', 5000);
    [peer] = await radarConnection;
    peer.on('error', () => {}); // Fail the scenario by its unmet assertions and still complete cleanup.
    const localUrl = 'ws://127.0.0.1:' + port;
    const via = 'wss://' + r.host;
    const local = await connect(t, localUrl + '/radar');
    const viewer = await connect(t, via + '/radar', { ca: r.tls.cert });
    const { ws: operator } = await r.browser();
    managedBridge(t, { localUrl: localUrl + '/radar', remoteUrl: via + '/ingest?key=' + r.key,
      reconnectMs: 80, ca: r.tls.cert, control: { remoteUrl: via + '/control-link',
        localUrl: localUrl + '/control-local', bridgeToken: r.bridgeToken, localToken: r.localToken, ca: r.tls.cert } });

    // Same wire layout and threshold trajectory as the existing tracking_radar_fixture.ps1.
    let frame = 100, y = 0.4;
    const send = () => {
      const packet = Buffer.alloc(52 + 40 * 24);
      packet.writeUInt32LE(2882356001, 4);
      packet.writeUInt32LE(16 + 40 * 24, 16);
      packet.writeBigUInt64LE(578437695752307201n, 36);
      packet.writeUInt32LE(frame++, 44);
      packet.writeUInt32LE(40, 48);
      for (let i = 0; i < 40; ++i) {
        const offset = 52 + i * 20;
        packet.writeFloatLE(0.25, offset);
        packet.writeFloatLE(y, offset + 4);
        packet.writeFloatLE(1, offset + 8);
        packet.writeFloatLE(10, offset + 16);
        packet.writeInt32LE(255, 52 + 40 * 20 + i * 4);
      }
      if (!peer.destroyed) peer.write(packet);
    };
    const feeding = setInterval(send, 125);
    registerCleanup(t, 'synthetic-radar-tick', () => clearInterval(feeding));
    await expectControl(operator, m => m.type === 'control_ready' && m.ready, 'arbitration control ready');
    await expectControl(viewer, m => m.door_state === 'OPEN', 'normal arrival opened the door');
    for (const step of [0.32, 0.24, 0.16, 0.08, 0, 0]) {
      y = step;
      await delay(135);
    }
    const hold = await expectControl(viewer, m => m.door_command === 'HOLD' &&
      m.passengers.some(person => ['ENTERING', 'EXITING'].includes(person.state)), 'live C threshold HOLD');
    assert.match(backend.output(), /C-request session=1 command=HOLD/);
    operatorRequest(operator, 'override-c-hold', 'CLOSE');
    await expectControl(operator, m => m.client_request_id === 'override-c-hold' && m.status === 'APPLIED', 'remote CLOSE over C HOLD');
    const closing = await expectControl(viewer, m => m.door_state === 'CLOSING' &&
      m.door_command === 'CLOSE' && m.timestamp_us >= hold.timestamp_us, 'remote closing');
    local.send('{"door_command":"OPEN"}');
    await until(() => backend.output().includes('remote priority'), 'local reversal rejected');
    const closed = await expectControl(viewer, m => m.door_state === 'CLOSED' &&
      m.timestamp_us > closing.timestamp_us, 'remote completion');
    local.send('{"door_command":"OPEN"}');
    await expectControl(viewer, m => m.door_state === 'OPENING' && m.door_command === 'OPEN' &&
      m.timestamp_us >= closed.timestamp_us, 'local command accepted after AUTO return');
    clearInterval(feeding);
    peer.end();
    assert.equal((await backend.finished).code, 0);
    console.log('live C HOLD overridden by remote CLOSE, local reversal blocked, AUTO return observed');
  });
}
