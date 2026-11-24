import { useEffect, useRef, useState } from 'react';

const noControl = { ready: false, reason: null };
const outcomes = {
  ACCEPTED: 'Relay 접수', APPLIED: '가상 문에 적용', REJECTED: '거부',
  EXPIRED: '만료', CANCELLED: '취소', UNKNOWN: '적용 여부 확인 불가',
};

export default function useRemoteDoorControl(enabled) {
  const secure = window.location.protocol === 'https:';
  const [connection, setConnection] = useState(false);
  const [readiness, setReadiness] = useState(noControl);
  const [generation, setGeneration] = useState(0);
  const [result, setResult] = useState('');
  const socket = useRef(null);
  const current = useRef(null);
  const timeout = useRef(null);

  useEffect(() => {
    if (!enabled || !secure) return undefined;
    let stopped = false;
    let timer;
    let authenticated = false;
    function connect() {
      const ws = new WebSocket('wss://' + window.location.host + '/control');
      socket.current = ws;
      ws.onopen = () => {
        if (stopped) return;
        authenticated = true;
        setConnection(true);
      };
      ws.onmessage = ({ data }) => {
        if (stopped) return;
        let msg;
        try { msg = JSON.parse(data); } catch { return; }
        if (msg?.type === 'control_ready' && typeof msg.ready === 'boolean') {
          setReadiness({ ready: msg.ready && msg.enabled === true, reason: msg.reason });
        } else if (msg?.type === 'door_result' && current.current?.id === msg.client_request_id &&
                   current.current.command === msg.door_command && Object.hasOwn(outcomes, msg.status)) {
          if (current.current.requestId && current.current.requestId !== msg.request_id) return;
          current.current.requestId = msg.request_id;
          setResult(outcomes[msg.status] + (msg.reason ? ' (' + msg.reason + ')' : ''));
          if (msg.status !== 'ACCEPTED') {
            clearTimeout(timeout.current);
            current.current = null;
          }
        }
      };
      ws.onclose = ({ code }) => {
        if (stopped) return;
        socket.current = null;
        setConnection(false);
        setReadiness(noControl);
        if (current.current) {
          clearTimeout(timeout.current);
          current.current = null;
          setResult(outcomes.UNKNOWN);
        }
        // Reconnect only a channel that has previously authenticated. Never resend.
        if (authenticated && code !== 1008) timer = setTimeout(connect, 2000);
      };
    }
    connect();
    return () => {
      stopped = true;
      clearTimeout(timer);
      clearTimeout(timeout.current);
      if (current.current) {
        current.current = null;
        setResult(outcomes.UNKNOWN);
      }
      if (socket.current) socket.current.close();
      socket.current = null;
      setConnection(false);
      setReadiness(noControl);
    };
  }, [enabled, generation, secure]);

  async function login(token) {
    if (!enabled || !secure) { setResult('관제 인증에는 HTTPS 연결이 필요합니다.'); return; }
    try {
      const response = await fetch('/control/login', {
        method: 'POST', credentials: 'same-origin', redirect: 'error', headers: { Authorization: 'Bearer ' + token },
      });
      if (!response.ok) { setResult('관제 인증 실패'); return; }
      setResult('관제 인증 완료');
      setGeneration(value => value + 1);
    } catch { setResult('관제 인증 연결 실패'); }
  }

  async function logout() {
    if (!enabled || !secure) return;
    try {
      const response = await fetch('/control/logout', { method: 'POST', credentials: 'same-origin', redirect: 'error' });
      if (response.ok) setGeneration(value => value + 1);
      else setResult('관제 세션 종료 확인 불가');
    } catch { setResult('관제 세션 종료 확인 불가'); }
  }

  function send(command) {
    if (!readiness.ready || socket.current?.readyState !== WebSocket.OPEN || current.current) return;
    const id = window.crypto.randomUUID();
    try {
      socket.current.send(JSON.stringify({ type: 'door_request', client_request_id: id, door_command: command }));
      current.current = { id, command };
      setResult('요청 송신');
      timeout.current = setTimeout(() => {
        current.current = null;
        setResult(outcomes.UNKNOWN);
      }, 12000);
    } catch { setResult('관제 요청 송신 실패'); }
  }

  return { secure, connection, readiness, result, login, logout, send };
}
