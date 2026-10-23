/*
 * fwsim live dashboard - browser side.
 *
 * The server streams what the firmware does (Server-Sent Events on /events).
 * This file turns that into an animation that obeys the same physics as
 * disk-sim:
 *
 *   - Platter angle is a pure function of simulated time:
 *       theta(t) = frac(t / rev_ms)
 *     exactly the rotational-position model disk-sim uses. A sector whose
 *     angular position is `a` passes under the head when theta == a.
 *   - Every media event arrives at the START of an access, with disk-sim's
 *     seek / rotation / transfer breakdown. We replay it in drive time:
 *     the arm swings for `seek` ms (accelerate, decelerate, settle), the
 *     target sector then rotates round to the head during `rot` ms, and is
 *     highlighted while it passes underneath for `xfer` ms.
 *   - Drive time maps to screen time by the time scale chosen in the UI
 *     (1 = real time, 10 = ten times slower). The firmware itself runs at
 *     that scale, so what you see is what the firmware is doing right now.
 */
'use strict';

const $ = (id) => document.getElementById(id);
const COL = { empty: '#1e2330', clean: '#1f6f63', dirty: '#f59e0b', lost: '#ef4444',
              read: '#60a5fa', write: '#fde68a' };
const STATE_COL = [COL.empty, COL.clean, COL.dirty, COL.lost];

let G = null;               // geometry from the server's hello
let grid = null;            // Uint8Array, one state per platter cell
let gridDirty = true;
let scale = 10;             // wall ms per drive ms
let phase = 'idle';
let powered = true;

/* ---- simulated clock --------------------------------------------------------- */

let clockOffset = null;     // simMs = perf / scale + clockOffset
const LAG_WALL_MS = 45;     // render slightly behind so events arrive first

function syncClock(t) {
  const est = t - performance.now() / scale;
  if (clockOffset === null || est > clockOffset || clockOffset - est > 250 / scale + 5)
    clockOffset = est;
}
function simNow() {
  if (clockOffset === null) return 0;
  return performance.now() / scale + clockOffset - LAG_WALL_MS / scale;
}

/* ---- event queues (played back in drive time) -------------------------------- */

const mediaQ = [];          // media accesses to animate
const cellsQ = [];          // grid updates, each effective at its own t
let cur = null;             // the access being animated
let lastSeek = { seek: null, rot: null, xfer: null };

/* ---- geometry of the drawing ----------------------------------------------------- */

const cv = $('platter');
const ctx = cv.getContext('2d');
let W = 0, H = 0, dpr = 1, C = { x: 0, y: 0 }, R = 0, R_OUT = 0, R_IN = 0, R_HUB = 0;
let P = { x: 0, y: 0 }, ARM = 0;
let headR = 0;              // current head radius (px)
let parked = true;
let gridCanvas = document.createElement('canvas');

function layout() {
  dpr = window.devicePixelRatio || 1;
  const rect = cv.getBoundingClientRect();
  W = rect.width; H = rect.height;
  cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
  R = Math.min(W * 0.43, H * 0.47);
  C = { x: W * 0.44, y: H * 0.5 };
  R_OUT = R * 0.97; R_IN = R * 0.36; R_HUB = R * 0.27;
  P = { x: C.x + R * 1.0, y: C.y + R * 0.88 };
  ARM = Math.hypot(P.x - C.x, P.y - C.y) * 0.96;
  if (!headR || parked) headR = R_OUT + R * 0.09;
  gridDirty = true;
}
window.addEventListener('resize', layout);

function cylRadius(cyl) {
  return R_OUT - (cyl / G.cylinders) * (R_OUT - R_IN);
}
function sectorAngle(cyl, sec, spt) {   // fraction of a revolution, as disk-sim
  const a = sec / spt + ((cyl * G.skew) % 1);
  return a - Math.floor(a);
}

/* Head position for a radius: rotary actuator around pivot P. */
function headPos(r) {
  const dx = C.x - P.x, dy = C.y - P.y, d = Math.hypot(dx, dy);
  const beta = Math.atan2(dy, dx);
  let c = (d * d + ARM * ARM - r * r) / (2 * d * ARM);
  c = Math.max(-1, Math.min(1, c));
  const phi = beta + Math.acos(c);
  return { x: P.x + ARM * Math.cos(phi), y: P.y + ARM * Math.sin(phi), phi };
}

/* Seek profile: bang-bang (accelerate, then decelerate) to the track in the
 * first 85% of the seek, then a damped settle onto the track centre - the
 * "settle" term in disk-sim's seek = settle + k * sqrt(distance). */
function seekShape(p) {
  if (p <= 0) return 0;
  if (p < 0.85) { const q = p / 0.85; return q < 0.5 ? 2 * q * q : 1 - 2 * (1 - q) * (1 - q); }
  if (p >= 1) return 1;
  const q = (p - 0.85) / 0.15;
  return 1 + 0.025 * Math.exp(-3.5 * q) * Math.sin(q * Math.PI * 3);
}

/* ---- platter angle, including spin-down after a power cut ------------------------- */

let spin = { off: false, theta0: 0, perf0: 0 };
function theta(now) {
  if (!G) return 0;
  if (!spin.off) return (now / G.rev_ms) % 1;
  // Power is gone: the spindle coasts down (exponential, ~0.8 s on screen).
  const dt = performance.now() - spin.perf0, tau = 800;
  const w0 = 1 / (G.rev_ms * scale);                 // rev per wall ms
  return (spin.theta0 + w0 * tau * (1 - Math.exp(-dt / tau))) % 1;
}

/* ---- drawing -------------------------------------------------------------------------- */

function drawGrid() {
  const size = Math.ceil(2 * R_OUT * dpr) + 4;
  gridCanvas.width = size; gridCanvas.height = size;
  const g = gridCanvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, size / 2, size / 2);
  const ringW = (R_OUT - R_IN) / G.rings;
  const gap = Math.min(0.012, 0.25 / G.cells);
  for (let ring = 0; ring < G.rings; ring++) {
    const r2 = R_OUT - ring * ringW, r1 = r2 - ringW + Math.min(1.5, ringW * 0.18);
    for (let s = 0; s < G.cells; s++) {
      const st = grid[ring * G.cells + s];
      const a1 = (s / G.cells + gap) * 2 * Math.PI, a2 = ((s + 1) / G.cells - gap) * 2 * Math.PI;
      g.beginPath();
      g.arc(0, 0, r2, a1, a2);
      g.arc(0, 0, r1, a2, a1, true);
      g.closePath();
      g.fillStyle = STATE_COL[st];
      g.fill();
    }
  }
  gridDirty = false;
}

function frame() {
  requestAnimationFrame(frame);
  if (!G || !W) return;
  const now = simNow();

  // Apply grid updates that are due.
  while (cellsQ.length && cellsQ[0].t <= now) applyCells(cellsQ.shift().u);
  // Start accesses that are due. If we fell far behind (background tab),
  // jump to the newest instead of replaying a backlog.
  if (mediaQ.length > 64) mediaQ.splice(0, mediaQ.length - 4);
  while (mediaQ.length && mediaQ[0].t <= now) beginAccess(mediaQ.shift());

  // Head radius.
  if (cur) {
    const tSeekEnd = cur.t + cur.seek;
    if (now < tSeekEnd && cur.seek > 0) headR = cur.r0 + (cur.r1 - cur.r0) * seekShape((now - cur.t) / cur.seek);
    else headR = cur.r1;
  }
  if (spin.off) headR += (R_OUT + R * 0.09 - headR) * 0.08;   // park on the ramp

  if (gridDirty) drawGrid();
  const hp = headPos(headR);
  const headAngle = Math.atan2(hp.y - C.y, hp.x - C.x);
  const th = theta(now);

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  // Platter body.
  ctx.beginPath(); ctx.arc(C.x, C.y, R, 0, 2 * Math.PI);
  ctx.fillStyle = '#0b0d11'; ctx.fill();
  ctx.strokeStyle = '#2d3340'; ctx.lineWidth = 2; ctx.stroke();

  // Rotating sector grid. A point at angular coordinate a is drawn at
  // headAngle + 2*pi*(a - theta): it reaches the head exactly when theta == a.
  const revPerFrameScreen = (16.7 / scale) / G.rev_ms;   // revolutions per frame
  const blur = !spin.off && revPerFrameScreen > 0.06 ? 3 : 1;
  for (let k = 0; k < blur; k++) {
    const rot = headAngle - 2 * Math.PI * (th - k * Math.min(revPerFrameScreen, 0.3) / blur * 0.5);
    ctx.save();
    ctx.globalAlpha = blur > 1 ? (k === 0 ? 0.6 : 0.25) : 1;
    ctx.translate(C.x, C.y); ctx.rotate(rot);
    ctx.drawImage(gridCanvas, -gridCanvas.width / (2 * dpr), -gridCanvas.height / (2 * dpr),
                  gridCanvas.width / dpr, gridCanvas.height / dpr);
    ctx.restore();
  }
  ctx.globalAlpha = 1;

  // The access in progress: target marker and the sectors passing under the head.
  let xferring = false, xferRead = false;
  if (cur) {
    const rot = headAngle - 2 * Math.PI * th;
    const tX0 = cur.t + cur.seek + cur.rot, tX1 = tX0 + cur.xfer;
    ctx.save(); ctx.translate(C.x, C.y); ctx.rotate(rot);
    const a0 = cur.a0 * 2 * Math.PI;
    if (now < tX0) {                               // target sector coming round
      const x = cur.r1 * Math.cos(a0), y = cur.r1 * Math.sin(a0);
      ctx.beginPath(); ctx.arc(x, y, 6, 0, 2 * Math.PI);
      ctx.strokeStyle = cur.w ? COL.write : COL.read; ctx.lineWidth = 2; ctx.stroke();
    } else if (now < tX1 + 30 / scale) {           // sectors passing under the head
      const p = Math.min(1, (now - tX0) / Math.max(cur.xfer, 1e-6));
      const span = Math.min(cur.span, 0.995) * 2 * Math.PI * p;
      ctx.beginPath(); ctx.arc(0, 0, cur.r1, a0, a0 + Math.max(span, 0.02));
      ctx.strokeStyle = cur.w ? COL.write : COL.read;
      ctx.lineWidth = Math.max(4, (R_OUT - R_IN) / G.rings * 1.6); ctx.lineCap = 'round'; ctx.stroke();
      xferring = now < tX1; xferRead = !cur.w;
    }
    ctx.restore();
  }

  // Hub.
  ctx.beginPath(); ctx.arc(C.x, C.y, R_HUB, 0, 2 * Math.PI);
  ctx.fillStyle = '#252a35'; ctx.fill(); ctx.strokeStyle = '#3a4150'; ctx.lineWidth = 2; ctx.stroke();
  ctx.save(); ctx.translate(C.x, C.y); ctx.rotate(-2 * Math.PI * th);
  for (let i = 0; i < 6; i++) {
    const a = i * Math.PI / 3;
    ctx.beginPath(); ctx.arc(R_HUB * 0.62 * Math.cos(a), R_HUB * 0.62 * Math.sin(a), R_HUB * 0.08, 0, 2 * Math.PI);
    ctx.fillStyle = '#0b0d11'; ctx.fill();
  }
  ctx.restore();
  ctx.beginPath(); ctx.arc(C.x, C.y, R_HUB * 0.22, 0, 2 * Math.PI); ctx.fillStyle = '#0b0d11'; ctx.fill();

  // Parking ramp.
  ctx.fillStyle = '#252a35';
  const rp = headPos(R_OUT + R * 0.09);
  ctx.beginPath(); ctx.arc(rp.x, rp.y, R * 0.05, 0, 2 * Math.PI); ctx.fill();

  // Actuator arm + head.
  ctx.lineCap = 'round';
  ctx.strokeStyle = '#c9cfda'; ctx.lineWidth = Math.max(8, R * 0.045);
  ctx.beginPath(); ctx.moveTo(P.x, P.y); ctx.lineTo(hp.x, hp.y); ctx.stroke();
  ctx.strokeStyle = '#7d8596'; ctx.lineWidth = Math.max(3, R * 0.014);
  ctx.beginPath(); ctx.moveTo(P.x, P.y); ctx.lineTo(hp.x, hp.y); ctx.stroke();
  if (xferring) {
    ctx.beginPath(); ctx.arc(hp.x, hp.y, R * 0.07, 0, 2 * Math.PI);
    ctx.fillStyle = xferRead ? 'rgba(96,165,250,0.28)' : 'rgba(245,158,11,0.30)'; ctx.fill();
  }
  ctx.save(); ctx.translate(hp.x, hp.y); ctx.rotate(hp.phi);
  const hs = Math.max(10, R * 0.045);
  ctx.fillStyle = xferring ? (xferRead ? COL.read : COL.dirty) : '#e7e9ee';
  ctx.fillRect(-hs * 0.6, -hs / 2, hs * 1.2, hs);
  ctx.restore();
  ctx.beginPath(); ctx.arc(P.x, P.y, R * 0.12, 0, 2 * Math.PI);
  ctx.fillStyle = '#252a35'; ctx.fill(); ctx.strokeStyle = '#3a4150'; ctx.lineWidth = 2; ctx.stroke();
  ctx.beginPath(); ctx.arc(P.x, P.y, R * 0.035, 0, 2 * Math.PI); ctx.fillStyle = '#8b93a3'; ctx.fill();

  if (!powered) {
    ctx.fillStyle = 'rgba(15,17,21,0.35)'; ctx.fillRect(0, 0, W, H);
  }
  drawLatency();
  flushLog();
}

function applyCells(u) {
  if (!grid) return;
  for (let i = 0; i < u.length; i += 2) grid[u[i]] = u[i + 1];
  gridDirty = true;
}

function beginAccess(ev) {
  const r1 = cylRadius(ev.cyl);
  cur = {
    t: ev.t, seek: ev.seek, rot: ev.rot, xfer: ev.xfer, w: ev.w,
    r0: headR, r1, a0: sectorAngle(ev.cyl, ev.sec, ev.spt), span: ev.n / ev.spt
  };
  parked = false;
  $('t-seek').textContent = fmtMs(ev.seek);
  $('t-rot').textContent = fmtMs(ev.rot);
  $('t-xfer').textContent = fmtMs(ev.xfer);
  $('headinfo').textContent = `head → cyl ${ev.cyl.toLocaleString()} · head ${ev.head} · LBA ${ev.lba.toLocaleString()}`;
  const why = ev.why === 'host' ? (ev.w ? 'WRITE' : 'READ ') : ev.why === 'destage' ? 'DESTAGE' : 'PREFETCH';
  log(`${why.padEnd(8)} LBA ${ev.lba.toLocaleString().padStart(11)} ×${ev.n}  cyl ${ev.cyl}  seek ${ev.seek.toFixed(2)} rot ${ev.rot.toFixed(2)} ms`,
      ev.w ? (ev.why === 'destage' ? 'd' : 'w') : 'r');
}

function fmtMs(x) { return x < 0.1 ? x.toFixed(3) + ' ms' : x < 10 ? x.toFixed(2) + ' ms' : x.toFixed(1) + ' ms'; }

/* ---- latency chart ---------------------------------------------------------------------- */

const latCv = $('lat'), latCtx = latCv.getContext('2d');
const lats = [];
let latDirty = true;
function drawLatency() {
  if (!latDirty) return;
  latDirty = false;
  const r = latCv.getBoundingClientRect(), d = window.devicePixelRatio || 1;
  if (latCv.width !== Math.round(r.width * d)) { latCv.width = Math.round(r.width * d); latCv.height = Math.round(r.height * d); }
  const w = r.width, h = r.height;
  latCtx.setTransform(d, 0, 0, d, 0, 0);
  latCtx.clearRect(0, 0, w, h);
  const lo = Math.log10(0.01), hi = Math.log10(1000);
  const y = (v) => h - 14 - (Math.log10(Math.max(v, 0.01)) - lo) / (hi - lo) * (h - 22);
  latCtx.font = '10px IBM Plex Mono, monospace'; latCtx.fillStyle = '#9aa3b2';
  for (const v of [0.01, 1, 100]) {
    latCtx.strokeStyle = '#222733'; latCtx.lineWidth = 1;
    latCtx.beginPath(); latCtx.moveTo(34, y(v)); latCtx.lineTo(w, y(v)); latCtx.stroke();
    latCtx.fillText(String(v), 0, y(v) + 3);
  }
  if (lats.length < 2) return;
  latCtx.strokeStyle = COL.dirty; latCtx.lineWidth = 2; latCtx.lineJoin = 'round';
  latCtx.beginPath();
  lats.forEach((v, i) => {
    const x = 36 + (i / 199) * (w - 40);
    if (i === 0) latCtx.moveTo(x, y(v)); else latCtx.lineTo(x, y(v));
  });
  latCtx.stroke();
}

/* ---- log ---------------------------------------------------------------------------------- */

const logLines = [];
let logDirty = false;
function log(text, cls) {
  logLines.unshift({ text, cls: cls || '' });
  if (logLines.length > 14) logLines.length = 14;
  logDirty = true;
}
function flushLog() {
  if (!logDirty) return;
  logDirty = false;
  const ol = $('log');
  ol.replaceChildren(...logLines.map((l) => {
    const li = document.createElement('li');
    li.textContent = l.text;
    if (l.cls) li.className = l.cls;
    return li;
  }));
}

/* ---- tags --------------------------------------------------------------------------------------- */

const tagEls = [];
for (let i = 0; i < 32; i++) {
  const el = document.createElement('div');
  el.className = 'tag'; el.textContent = i;
  $('tags').appendChild(el); tagEls.push(el);
}
function setTags(s) {
  for (let i = 0; i < 32; i++) tagEls[i].className = 'tag' + (s[i] === 'q' ? ' q' : s[i] === 'a' ? ' a' : '');
}

/* ---- stats ------------------------------------------------------------------------------------------- */

const fmt = (x, d = 1) => (x === undefined || x === null || x < 0) ? '–' : x.toLocaleString(undefined, { minimumFractionDigits: d, maximumFractionDigits: d });
function onStats(s) {
  $('s-iops').textContent = fmt(s.iops, 1);
  $('s-p50').textContent = s.cmds ? fmt(s.p50, s.p50 < 1 ? 3 : 1) : '–';
  $('s-p99').textContent = s.cmds ? fmt(s.p99, s.p99 < 1 ? 3 : 1) : '–';
  $('s-avg').textContent = 'avg ' + (s.cmds ? fmt(s.avg, s.avg < 1 ? 3 : 1) + ' ms' : '–');
  $('s-p95').textContent = 'p95 ' + (s.cmds ? fmt(s.p95, s.p95 < 1 ? 3 : 1) + ' ms' : '–');
  $('s-hit').textContent = s.hit < 0 ? '–' : fmt(s.hit, 1) + '%';
  $('s-busy').textContent = 'media busy ' + fmt(s.busy, 0) + '%';
  const pct = s.cache ? (100 * s.dirty / s.cache) : 0;
  $('m-fill').style.width = pct.toFixed(1) + '%';
  $('m-meter').setAttribute('aria-valuenow', String(s.dirty));
  $('m-text').textContent = `${s.dirty.toLocaleString()} / ${s.cache.toLocaleString()} dirty sectors`;
  $('m-counts').textContent = `destage runs ${s.destage_runs.toLocaleString()} · commands ${s.cmds.toLocaleString()} · acked writes ${s.acked.toLocaleString()} · CRC errors ${s.crc} (recovered ${s.crc_ok}, failed ${s.crc_bad})`;
  setTags(s.tags);
}

/* ---- state / buttons -------------------------------------------------------------------------------- */

function setPhase(p) {
  phase = p;
  const pill = $('status'), text = $('status-text');
  const map = { idle: ['pill-idle', 'IDLE · platters spinning'], running: ['pill-run', 'RUNNING'],
                stopping: ['pill-run', 'STOPPING'], off: ['pill-off', 'POWER OFF'],
                verifying: ['pill-verify', 'REBOOTING · VERIFYING'] };
  const [cls, label] = map[p] || map.idle;
  pill.className = 'pill ' + cls; text.textContent = label;
  $('btn-run').disabled = p !== 'idle';
  $('btn-stop').disabled = p !== 'running';
  $('btn-flush').disabled = !(p === 'idle' || p === 'running');
  $('btn-plug').disabled = !(p === 'idle' || p === 'running');
  $('btn-reset').disabled = !(p === 'idle');
}

const api = (path) => fetch('/api/' + path).catch(() => {});
$('btn-run').addEventListener('click', () => {
  const q = new URLSearchParams({
    workload: $('c-workload').value, mode: $('c-mode').value, qd: $('c-qd').value,
    sorted: $('c-sorted').value, region: $('c-region').value, flip: $('c-flip').value,
    scale: $('c-speed').value
  });
  api('start?' + q.toString());
});
$('btn-stop').addEventListener('click', () => api('stop'));
$('btn-flush').addEventListener('click', () => api('flush'));
$('btn-plug').addEventListener('click', () => api('cut'));
$('btn-reset').addEventListener('click', () => api('reset'));

function updateMotorNote() {
  const s = scale;
  $('motor-note').textContent = s === 1
    ? 'Spindle at the real 7,200 rpm (120 revolutions per second): too fast to follow, which is the point. Timings are disk-sim\'s, per access, in drive time.'
    : `Spindle 7,200 rpm shown at 1/${s} speed (${Math.round(7200 / s).toLocaleString()} rpm on screen). Seek, rotation and transfer are disk-sim's, in drive time.`;
}

/* ---- the event stream ------------------------------------------------------------------------------------ */

function connect() {
  const es = new EventSource('/events');
  es.onmessage = (m) => {
    let ev;
    try { ev = JSON.parse(m.data); } catch { return; }
    switch (ev.type) {
      case 'hello':
        G = ev;
        grid = new Uint8Array(G.rings * G.cells);
        for (let i = 0; i < grid.length; i++) grid[i] = ev.grid.charCodeAt(i) - 48;
        clockOffset = null; syncClock(ev.sim);
        $('geo').textContent = `7,200 rpm · ${(G.capacity * 512 / 1e9).toFixed(0)} GB · ${(G.cache * 512 / 1048576).toFixed(0)} MiB cache · queue ${G.queue}`;
        layout();
        break;
      case 'state':
        scale = ev.scale; setPhase(ev.phase); updateMotorNote();
        powered = ev.phase !== 'off' && ev.phase !== 'verifying';
        break;
      case 'media':
        syncClock(ev.t); mediaQ.push(ev);
        break;
      case 'cells':
        if (!ev.t) { applyCells(ev.u); break; }   // t = 0: effective now (verify)
        syncClock(ev.t);
        cellsQ.push(ev); cellsQ.sort((a, b) => a.t - b.t);
        break;
      case 'start':
        syncClock(ev.t);
        break;
      case 'done':
        lats.push(ev.lat); if (lats.length > 200) lats.shift(); latDirty = true;
        break;
      case 'crc':
        log(`CRC     LBA ${ev.lba.toLocaleString()} ${ev.ok ? 'mismatch → retry OK' : 'unrecoverable → MEDIUM ERROR'}`, 'x');
        break;
      case 'stats':
        syncClock(ev.t); onStats(ev);
        break;
      case 'power':
        if (!ev.on) {
          // Everything queued happened before the cut: apply it now, so the
          // verification result that follows is the last word.
          while (cellsQ.length) applyCells(cellsQ.shift().u);
          spin = { off: true, theta0: theta(simNow()), perf0: performance.now() };
          powered = false; mediaQ.length = 0; cur = null;
          log(`POWER CUT · ${ev.dirty} dirty sectors in DRAM discarded${ev.torn ? ' · torn write' : ''}`, 'x');
          $('verify-text').textContent = 'Power cut. Rebooting and reading every acknowledged block back...';
        } else {
          spin.off = false; powered = true; clockOffset = null;
          log('POWER ON · cold cache, spindle up to 7,200 rpm', 'd');
        }
        break;
      case 'verify': showVerify(ev); break;
      case 'reset':
        if (grid) grid.fill(0); gridDirty = true; lats.length = 0; latDirty = true;
        log('DISK WIPED · new blank media', 'd');
        $('verify-text').textContent = 'Pull the plug during a write workload to see which acknowledged writes survive.';
        break;
    }
  };
  es.onerror = () => { setPhase('idle'); $('status-text').textContent = 'RECONNECTING'; };
}

function showVerify(v) {
  const box = $('verify-text');
  const lostPct = v.checked ? (100 * v.lost / v.checked) : 0;
  box.replaceChildren();
  const big = document.createElement('div');
  big.className = 'verify-big ' + (v.lost ? 'verify-bad' : 'verify-good');
  big.textContent = v.checked ? `${v.lost.toLocaleString()} of ${v.checked.toLocaleString()} lost` : 'nothing to check';
  const p = document.createElement('p');
  p.className = 'muted small';
  p.textContent = `Mode: ${v.mode}. ${v.lost ? `${lostPct.toFixed(1)}% of acknowledged blocks were only in the DRAM cache (${v.dirty_at_cut.toLocaleString()} dirty sectors at the cut).` : 'Every acknowledged write was on the platters.'} Durability promises broken (FUA, write-through or a completed FLUSH): ${v.violations}.`;
  box.append(big, p);
  log(`VERIFY  ${v.checked} blocks checked · ${v.lost} lost · ${v.violations} promises broken`, v.lost ? 'x' : 'd');
}

setPhase('idle');
layout();
connect();
requestAnimationFrame(frame);
