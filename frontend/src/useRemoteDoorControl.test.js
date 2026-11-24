import { act, renderHook } from '@testing-library/react';
import useRemoteDoorControl from './useRemoteDoorControl';

const sockets = [];

class ControlSocket {
  static OPEN = 1;

  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.send = jest.fn();
    sockets.push(this);
  }

  open() {
    this.readyState = ControlSocket.OPEN;
    this.onopen();
  }

  receive(message) {
    this.onmessage({ data: JSON.stringify(message) });
  }

  close() {
    this.readyState = 3;
    this.onclose?.({ code: 1006 });
  }
}

let originalWebSocket;
let originalCrypto;
let originalLocation;

beforeEach(() => {
  jest.useFakeTimers();
  sockets.length = 0;
  originalWebSocket = global.WebSocket;
  global.WebSocket = ControlSocket;
  originalLocation = Object.getOwnPropertyDescriptor(window, 'location');
  originalCrypto = Object.getOwnPropertyDescriptor(window, 'crypto');
  Object.defineProperty(window, 'location', { configurable: true, value: { protocol: 'https:', host: '127.0.0.1' } });
  const randomUUID = jest.fn().mockReturnValueOnce('frontend-test-request').mockReturnValueOnce('frontend-next-request');
  Object.defineProperty(window, 'crypto', { configurable: true, value: { randomUUID } });
});

afterEach(() => {
  global.WebSocket = originalWebSocket;
  Object.defineProperty(window, 'location', originalLocation);
  if (originalCrypto) Object.defineProperty(window, 'crypto', originalCrypto);
  else delete window.crypto;
  jest.useRealTimers();
});

test('shows the Relay and Backend outcomes separately and never resends an uncertain command', () => {
  const { result } = renderHook(() => useRemoteDoorControl(true));
  const ws = sockets[0];
  expect(ws.url).toBe('wss://127.0.0.1/control');
  act(() => ws.open());
  act(() => result.current.send('OPEN'));
  expect(ws.send).not.toHaveBeenCalled();
  act(() => ws.receive({ type: 'control_ready', enabled: true, ready: true, reason: null }));
  act(() => result.current.send('OPEN'));
  expect(JSON.parse(ws.send.mock.calls[0][0])).toEqual({
    type: 'door_request', client_request_id: 'frontend-test-request', door_command: 'OPEN',
  });
  expect(result.current.result).toBe('요청 송신');

  const reply = { type: 'door_result', client_request_id: 'frontend-test-request',
    request_id: 'relay-1', door_command: 'OPEN', reason: null };
  act(() => ws.receive({ ...reply, status: 'ACCEPTED' }));
  expect(result.current.result).toBe('Relay 접수');
  act(() => ws.receive({ ...reply, request_id: 'other-result', status: 'APPLIED' }));
  expect(result.current.result).toBe('Relay 접수');
  act(() => ws.receive({ ...reply, status: 'APPLIED' }));
  expect(result.current.result).toBe('가상 문에 적용');

  act(() => result.current.send('HOLD'));
  act(() => ws.close());
  expect(result.current.result).toBe('적용 여부 확인 불가');
  expect(result.current.readiness.ready).toBe(false);
  act(() => jest.advanceTimersByTime(2000));
  expect(sockets).toHaveLength(2);
  expect(sockets[1].send).not.toHaveBeenCalled();
});

test('opens no control connection in viewer mode', () => {
  const { result } = renderHook(() => useRemoteDoorControl(false));
  act(() => result.current.send('CLOSE'));
  expect(sockets).toHaveLength(0);
});

test('does not send operator credentials from an HTTP page', async () => {
  window.location.protocol = 'http:';
  const { result } = renderHook(() => useRemoteDoorControl(true));
  const savedFetch = global.fetch;
  global.fetch = jest.fn();
  try {
    await act(async () => result.current.login('test-only-token'));
    expect(global.fetch).not.toHaveBeenCalled();
    expect(sockets).toHaveLength(0);
  } finally {
    global.fetch = savedFetch;
  }
});
