// docs/capture_dashboard.mjs - capture real screenshots of the dashboard,
// frame by frame, for the README animation.
//
// Starts a headless Chromium-family browser (Edge or Chrome), opens the
// dashboard (by default the recorded-session replay, so every run of this
// script captures the same real session), and grabs screenshots over the
// DevTools protocol. docs/make_gif.py turns the frames into a GIF.
//
//   node docs/capture_dashboard.mjs [url] [seconds] [outdir]
//
// Needs Node 22+ (built-in fetch and WebSocket) and fwsim-ui running
// (make run-ui).
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const URL_ = process.argv[2] || 'http://localhost:8080/?replay=1';
const SECONDS = Number(process.argv[3] || 22);
const OUT = process.argv[4] || 'docs/frames';
const PORT = 9333;
const W = 1360, H = 1260;

const candidates = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
];
const exe = candidates.find((p) => existsSync(p));
if (!exe) { console.error('no Chromium-family browser found'); process.exit(1); }

mkdirSync(OUT, { recursive: true });
const profile = mkdtempSync(join(tmpdir(), 'fwsim-cap-'));
const browser = spawn(exe, ['--headless=new', `--remote-debugging-port=${PORT}`,
  `--user-data-dir=${profile}`, `--window-size=${W},${H}`, '--hide-scrollbars',
  '--no-first-run', '--no-default-browser-check', 'about:blank'], { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let targets;
for (let i = 0; i < 50; i++) {
  try { targets = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); break; }
  catch { await sleep(200); }
}
const page = targets.find((t) => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let nextId = 1;
const pending = new Map();
ws.addEventListener('message', (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
});
const send = (method, params = {}) => new Promise((resolve) => {
  const id = nextId++;
  pending.set(id, resolve);
  ws.send(JSON.stringify({ id, method, params }));
});

await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
await send('Page.enable');
await send('Page.navigate', { url: URL_ });
await sleep(1200);                       // fonts + first events

const t0 = Date.now();
const times = [];
let n = 0;
while (Date.now() - t0 < SECONDS * 1000) {
  const shot = await send('Page.captureScreenshot', { format: 'png' });
  const name = join(OUT, `f${String(n).padStart(4, '0')}.png`);
  writeFileSync(name, Buffer.from(shot.result.data, 'base64'));
  times.push(Date.now() - t0);
  n++;
  const next = t0 + n * 100;             // aim for 10 frames per second
  if (next > Date.now()) await sleep(next - Date.now());
}
writeFileSync(join(OUT, 'times.json'), JSON.stringify(times));
console.log(`captured ${n} frames in ${((Date.now() - t0) / 1000).toFixed(1)} s -> ${OUT}`);
ws.close();
browser.kill();
