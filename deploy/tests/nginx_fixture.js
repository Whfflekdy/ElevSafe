'use strict';

// Linux integration fixture. Run the deployment locations on an ephemeral loopback
// TLS port, using a private prefix rather than changing the system nginx or trust store.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const https = require('node:https');
const { once } = require('node:events');
const { execFileSync, spawn } = require('node:child_process');
const { setTimeout: delay } = require('node:timers/promises');

module.exports = async function startNginx(t, { tls, relayPort, publicPort, httpPort, proxyToken, registerCleanup }) {
  const password = process.env.ELEVSAFE_TEST_PASSWORD;
  if (tls && !password) throw new Error('Isolated nginx test password required');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'elevsafe-nginx-'));
  registerCleanup(t, 'nginx-prefix', () => fs.rmSync(dir, { recursive: true, force: true }));
  const write = (name, data) => fs.writeFileSync(path.join(dir, name), data, { mode: 0o600 });
  if (tls) {
    write('tls_chain', tls.cert);
    write('tls_key', tls.key);
    const users = execFileSync(process.env.ELEVSAFE_TEST_HTPASSWD || 'htpasswd', ['-niB', 'admin'],
      { input: password + '\n', stdio: ['pipe', 'pipe', 'pipe'] });
    write('dashboard_users', users);
  }
  write('proxy.inc', 'set $elevsafe_proxy "' + proxyToken + '";\n');
  const root = process.env.ELEVSAFE_TEST_DASHBOARD || dir + '/html';
  if (!process.env.ELEVSAFE_TEST_DASHBOARD) {
    fs.mkdirSync(root);
    fs.writeFileSync(path.join(root, 'index.html'), 'fixture dashboard');
  }
  const config = name => fs.readFileSync(path.join(__dirname, '../web', name), 'utf8');
  let gateway = config('nginx.conf');
  if (httpPort) gateway = gateway.replace('listen 80;', 'listen 127.0.0.1:' + httpPort + ';');
  else gateway = gateway.slice(0, gateway.indexOf('server {'));
  if (tls) gateway += config('https.conf').replace('listen 443 ssl;', 'listen 127.0.0.1:' + publicPort + ' ssl;');
  const locations = gateway.replaceAll('include /etc/nginx/elevsafe-gateway.inc;', () => config('gateway.inc'))
    .replaceAll('/run/secrets/', dir + '/').replaceAll('/etc/nginx/elevsafe-proxy.inc', dir + '/proxy.inc')
    .replaceAll('/usr/share/nginx/html', '"' + root + '"').replaceAll('http://relay:8090', 'http://127.0.0.1:' + relayPort);
  const tempPaths = ['client_body', 'proxy', 'fastcgi', 'uwsgi', 'scgi']
    .map(name => name + '_temp_path "' + dir + '/' + name + '";').join('\n');
  write('nginx.conf', 'worker_processes 1; pid nginx.pid; error_log stderr crit; events {} http {\n' +
    'access_log off; types { text/html html; application/javascript js; text/css css; }\n' + tempPaths + '\n' + locations + '\n}');
  const proc = spawn(process.env.ELEVSAFE_TEST_NGINX, ['-p', dir + '/', '-c', dir + '/nginx.conf', '-g', 'daemon off;'],
    { stdio: ['ignore', 'ignore', 'pipe'] });
  proc.stderr.resume();
  const stopped = new Promise(resolve => { proc.once('error', resolve); proc.once('exit', resolve); });
  registerCleanup(t, 'nginx', async () => {
    if (proc.exitCode === null && proc.signalCode === null) proc.kill();
    await stopped;
  });
  await once(proc, 'spawn');
  for (let i = 0; i < 100; i++) {
    if (proc.exitCode !== null) throw new Error('nginx startup failed, exit ' + proc.exitCode);
    const status = await new Promise((resolve, reject) => {
      const req = (tls ? https : http).get({ host: '127.0.0.1', port: tls ? publicPort : httpPort, ca: tls?.ca || tls?.cert }, res => {
        res.resume(); resolve(res.statusCode);
      });
      req.once('error', e => e.code === 'ECONNREFUSED' ? resolve(null) : reject(e));
    });
    if (status === (tls ? 401 : 200)) return;
    if (status !== null) throw new Error('Missing nginx authentication challenge');
    await delay(25);
  }
  throw new Error('nginx readiness timeout');
};
