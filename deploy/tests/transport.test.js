'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const { once } = require('node:events');
const { createRequire } = require('node:module');
const requireRelay = createRequire(require.resolve('../relay/package.json'));
const WebSocket = requireRelay('ws');
const { WebSocketServer } = WebSocket;
const { createRelay } = require('../relay/relay');
const { createBridge } = require('../bridge/bridge');
const { limits, snapshotText, sendBounded } = require('../relay/transport');
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
async function rejectCode(t, url, expected) {
  const ws = client(t, url);
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
