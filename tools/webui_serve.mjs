/* te2_webui_serve — the Remote UI, on a laptop, driven by the real plugin.
 *
 *   node tools/webui_serve.mjs [--port 7700] [--module build-mac/tape-echo2.dylib]
 *   open http://localhost:7700/
 *
 * Serves src/web_ui.html and speaks the same /ws/remote-ui protocol that
 * schwung-manager does (subscribe / slot_info / chain_params / param_update /
 * set_param), so the page runs UNMODIFIED — the standalone branch of its
 * transport, exactly as it behaves when opened directly on a Move.
 *
 * Behind the socket is tools/webui_bridge.cpp with a NATIVE build of the
 * plugin, so a macro write really runs te2_apply_macro and the members it
 * drives really move on screen. Nothing here mocks the DSP.
 *
 * Zero dependencies: the WebSocket handshake and framing are inline below,
 * which is only tolerable because this speaks to one browser tab on loopback.
 */
import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const arg = (name, def) => {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = parseInt(arg('--port', '7700'), 10);
const MODULE = path.resolve(ROOT, arg('--module', 'build-mac/tape-echo2.dylib'));
const BRIDGE = path.resolve(ROOT, 'build-mac/te2_webui_bridge');
const COMPONENT = arg('--component', 'fx1');
const MODULE_ID = 'tape-echo2';

for (const p of [MODULE, BRIDGE]) {
  if (!fs.existsSync(p)) {
    console.error(`missing ${p}\n\nBuild both first:\n  ${buildHint()}`);
    process.exit(1);
  }
}
function buildHint() {
  return [
    'mkdir -p build-mac',
    'clang++ -std=c++17 -O2 -fPIC -shared -Isrc -Isrc/host -Isrc/ported/core \\',
    '  -Isrc/ported/shared-dpf/dsp src/dsp/tape_echo_plugin.cpp \\',
    '  src/ported/core/TapeEchoDSP.cpp -o build-mac/tape-echo2.dylib',
    'clang++ -std=c++17 -O2 -Isrc -Isrc/host tools/webui_bridge.cpp \\',
    '  -o build-mac/te2_webui_bridge',
  ].join('\n  ');
}

/* ---------------- the plugin, behind a line protocol ---------------- */

const proc = spawn(BRIDGE, [MODULE], { stdio: ['pipe', 'pipe', 'inherit'] });
proc.on('exit', (c) => { console.error(`bridge exited (${c})`); process.exit(1); });

let pending = [];
let buf = '';
proc.stdout.on('data', (d) => {
  buf += d.toString();
  let nl;
  while ((nl = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, nl);
    buf = buf.slice(nl + 1);
    const resolve = pending.shift();
    if (resolve) resolve(line);
  }
});
function ask(cmd) {
  return new Promise((resolve) => {
    pending.push(resolve);
    proc.stdin.write(cmd + '\n');
  });
}
const dump = () => ask('D').then(JSON.parse);
const chainParams = () => ask('P');
const setParam = (k, v) => ask(`S ${k} ${v}`).then(JSON.parse);

/* ---------------- HTTP ---------------- */

const server = http.createServer((req, res) => {
  const url = req.url.split('?')[0];
  if (url === '/' || url === '/web_ui.html') {
    const html = fs.readFileSync(path.join(ROOT, 'src/web_ui.html'));
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    res.end(html);
    return;
  }
  res.writeHead(404).end('not found');
});

/* ---------------- WebSocket (text frames only) ---------------- */

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const clients = new Set();

server.on('upgrade', (req, sock) => {
  if (req.url.split('?')[0] !== '/ws/remote-ui') { sock.destroy(); return; }
  const accept = crypto.createHash('sha1')
    .update(req.headers['sec-websocket-key'] + GUID).digest('base64');
  sock.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  sock.setNoDelay(true);
  clients.add(sock);
  sock.on('close', () => clients.delete(sock));
  sock.on('error', () => clients.delete(sock));

  let rx = Buffer.alloc(0);
  sock.on('data', (d) => {
    rx = Buffer.concat([rx, d]);
    for (;;) {
      const frame = decode(rx);
      if (!frame) break;
      rx = rx.slice(frame.consumed);
      if (frame.opcode === 0x8) { sock.end(); return; }
      if (frame.opcode === 0x1) onMessage(sock, frame.payload);
    }
  });
});

function decode(b) {
  if (b.length < 2) return null;
  const opcode = b[0] & 0x0f;
  const masked = (b[1] & 0x80) !== 0;
  let len = b[1] & 0x7f;
  let off = 2;
  if (len === 126) { if (b.length < 4) return null; len = b.readUInt16BE(2); off = 4; }
  else if (len === 127) { if (b.length < 10) return null; len = Number(b.readBigUInt64BE(2)); off = 10; }
  const maskLen = masked ? 4 : 0;
  if (b.length < off + maskLen + len) return null;
  const mask = masked ? b.slice(off, off + 4) : null;
  const data = Buffer.from(b.slice(off + maskLen, off + maskLen + len));
  if (mask) for (let i = 0; i < data.length; i++) data[i] ^= mask[i % 4];
  return { opcode, payload: data.toString('utf8'), consumed: off + maskLen + len };
}

function send(sock, obj) {
  const data = Buffer.from(JSON.stringify(obj), 'utf8');
  let head;
  if (data.length < 126) head = Buffer.from([0x81, data.length]);
  else if (data.length < 65536) {
    head = Buffer.alloc(4);
    head[0] = 0x81; head[1] = 126; head.writeUInt16BE(data.length, 2);
  } else {
    head = Buffer.alloc(10);
    head[0] = 0x81; head[1] = 127; head.writeBigUInt64BE(BigInt(data.length), 2);
  }
  sock.write(Buffer.concat([head, data]));
}
const broadcast = (obj) => { for (const c of clients) send(c, obj); };

/* ---------------- the manager's protocol ---------------- */

const prefixed = (vals) => {
  const o = {};
  for (const k of Object.keys(vals)) o[`${COMPONENT}:${k}`] = vals[k];
  return o;
};

async function onMessage(sock, text) {
  let m;
  try { m = JSON.parse(text); } catch { return; }
  const slot = m.slot ?? 0;

  if (m.type === 'subscribe') {
    /* slot_info is how the page discovers WHICH component it is driving */
    send(sock, { type: 'slot_info', slot, synth: '', [COMPONENT]: MODULE_ID });
    send(sock, { type: 'chain_params', slot, component: COMPONENT, data: await chainParams() });
    send(sock, { type: 'param_update', slot, params: prefixed(await dump()) });
    return;
  }

  if (m.type === 'set_param') {
    const key = String(m.key || '').replace(/^[^:]*:/, '');
    /* The full dump goes back to EVERY client, including the one that wrote:
     * a macro write moves params the writer never touched, and the members
     * moving on screen is the thing being verified here. */
    broadcast({ type: 'param_update', slot, params: prefixed(await setParam(key, m.value)) });
  }
}

server.listen(PORT, () => {
  console.log(`Tape Echo 2 remote UI  ->  http://localhost:${PORT}/`);
  console.log(`  module:    ${MODULE}`);
  console.log(`  component: ${COMPONENT}`);
});
