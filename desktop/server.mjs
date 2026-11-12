#!/usr/bin/env node
// ByOS desktop scaffold: a tiny local runner. It serves the UI in ui/ on 127.0.0.1 and runs
// byos-cli for it (lib/cli.mjs). Node.js only, no packages. See README.md.
//
//   node server.mjs [--mock] [--cli PATH] [--port N] [--open] [--config DIR] [--adb PATH]

import { randomBytes, timingSafeEqual } from 'node:crypto';
import {
  createWriteStream, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, utimesSync,
} from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { buildArgs, DESKTOP_DIR, Job, resolveCli, UsageError } from './lib/cli.mjs';
import { checkPath } from './lib/btp-path.mjs';

const UI_DIR = path.join(DESKTOP_DIR, 'ui');
const STATIC = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/i18n.js': ['i18n.js', 'text/javascript; charset=utf-8'],
  '/app.css': ['app.css', 'text/css; charset=utf-8'],
  '/icon.svg': ['icon.svg', 'image/svg+xml'],
  // The same path rules as the runner, shared with the browser.
  '/btp-path.js': ['../lib/btp-path.mjs', 'text/javascript; charset=utf-8'],
};
const MAX_UPLOAD = 4 * 1024 ** 3 - 1; // FAT32 file limit
const JOB_KEEP_MS = 10 * 60 * 1000;
const MAX_BODY = 64 * 1024;

const SECURITY_HEADERS = {
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'Cache-Control': 'no-store',
  'Content-Security-Policy':
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'",
};

function safeEqual(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  const x = Buffer.from(a);
  const y = Buffer.from(b);
  return x.length === y.length && timingSafeEqual(x, y);
}

// Removes temporary upload folders left by runners that are no longer alive (a crash, or a
// kill during an upload). Each runner names its folders byos-desktop-<pid>-....
export function sweepStaleTemp(tmpdir = os.tmpdir()) {
  let removed = 0;
  let names = [];
  try { names = readdirSync(tmpdir); } catch { return 0; }
  for (const name of names) {
    const full = path.join(tmpdir, name);
    const m = /^byos-desktop-(\d+)-[A-Za-z0-9]{6}$/.exec(name);
    if (m) {
      if (!pidGone(Number(m[1]))) continue;
    } else if (/^byos-desktop-[A-Za-z0-9]{6}$/.test(name)) {
      // The scaffold's first naming had no pid: only folders older than a day.
      try { if (Date.now() - statSync(full).mtimeMs < 86400000) continue; } catch { continue; }
    } else {
      continue;
    }
    try { rmSync(full, { recursive: true, force: true }); removed++; } catch { /* in use */ }
  }
  return removed;
}

function pidGone(pid) {
  if (pid === process.pid) return false;
  try {
    process.kill(pid, 0);
    return false;
  } catch (e) {
    return e.code === 'ESRCH';
  }
}

function send(res, status, body, type = 'application/json; charset=utf-8') {
  const data = typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': type, ...SECURITY_HEADERS });
  res.end(data);
}

async function readJson(req) {
  let size = 0;
  const chunks = [];
  for await (const c of req) {
    size += c.length;
    if (size > MAX_BODY) throw new UsageError('request too large');
    chunks.push(c);
  }
  if (!size) return {};
  try {
    const v = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    if (v === null || typeof v !== 'object' || Array.isArray(v)) throw new Error();
    return v;
  } catch {
    throw new UsageError('body must be a JSON object');
  }
}

export function createApp(options = {}) {
  const token = options.token ?? randomBytes(24).toString('hex');
  const resolved = resolveCli({ cli: options.cli, mock: options.mock, env: options.env });
  const globals = { config: options.config, adb: options.adb };
  const jobs = new Map();
  // One queue per device spec: hibysync serves one session, so jobs for the same HiBy run one
  // at a time, while another HiBy is not held up. "devices" (discovery) needs no session and
  // runs at once. The spec is the text the UI sent ("" = byos-cli's default device).
  const queues = new Map(); // spec -> { running, waiting: [] }
  let port = 0;
  const jobOptions = {
    timeoutMs: options.timeoutMs, opTimeoutsMs: options.opTimeoutsMs,
    promptTimeoutMs: options.promptTimeoutMs, killGraceMs: options.killGraceMs, env: options.env,
  };
  for (const k of Object.keys(jobOptions)) if (jobOptions[k] === undefined) delete jobOptions[k];

  function pump(spec) {
    const q = queues.get(spec);
    if (!q || q.running) return;
    const job = q.waiting.shift();
    if (!job) { queues.delete(spec); return; }
    q.running = job;
    job.once('end', () => {
      q.running = null;
      pump(spec);
    });
    job.start();
  }

  function startJob(command, params, cleanup) {
    if (resolved.error) throw Object.assign(new Error(resolved.error), { status: 503 });
    const args = buildArgs(command, params, globals);
    const job = new Job(resolved, args, jobOptions);
    job.command = command;
    job.spec = typeof params.device === 'string' ? params.device : '';
    job.state = 'queued';
    if (cleanup) job.once('end', cleanup);
    // Every job, however it ends (run, cancelled while queued, shut down), is forgotten later.
    job.once('end', () => setTimeout(() => jobs.delete(job.id), options.jobKeepMs ?? JOB_KEEP_MS).unref());
    jobs.set(job.id, job);
    if (command === 'devices') {
      job.start();
      return job;
    }
    if (!queues.has(job.spec)) queues.set(job.spec, { running: null, waiting: [] });
    queues.get(job.spec).waiting.push(job);
    pump(job.spec);
    return job;
  }

  // Ends a job that never started (cancel while queued, shutdown).
  function dropQueued(job, message) {
    const q = queues.get(job.spec);
    if (q) {
      const i = q.waiting.indexOf(job);
      if (i >= 0) q.waiting.splice(i, 1);
    }
    job.state = 'done';
    job.final = { v: 1, event: 'error', t: 0, ok: false, code: 'cancelled', message };
    job.events.push(job.final);
    job.emit('event', job.final);
    job.emit('end', job.final);
  }

  function hostOk(req) {
    const host = req.headers.host ?? '';
    return host === `127.0.0.1:${port}` || host === `localhost:${port}`;
  }

  // The cookie name carries the port, because browsers share cookies between ports.
  const cookieName = () => `byos_${port}`;

  function cookieOk(req) {
    const want = `${cookieName()}=${token}`;
    return (req.headers.cookie ?? '').split(/;\s*/).some((c) => safeEqual(c, want));
  }

  function tokenOk(req, url) {
    return safeEqual(req.headers['x-byos-token'], token) ||
      safeEqual(url.searchParams.get('token'), token) || cookieOk(req);
  }

  async function handle(req, res) {
    const url = new URL(req.url, 'http://127.0.0.1');
    if (!hostOk(req)) return send(res, 421, { error: 'wrong host' });

    // The page itself holds the token, so it is only served to whoever already has it: the
    // window opens /?t=TOKEN, which sets an HttpOnly cookie and redirects to /.
    if (req.method === 'GET' && url.pathname === '/') {
      if (url.searchParams.has('t')) {
        if (!safeEqual(url.searchParams.get('t'), token)) return send(res, 403, 'Wrong or old address. Use the one the runner printed.', 'text/plain; charset=utf-8');
        res.writeHead(303, {
          Location: '/',
          'Set-Cookie': `${cookieName()}=${token}; HttpOnly; SameSite=Strict; Path=/`,
          ...SECURITY_HEADERS,
        });
        return res.end();
      }
      if (!cookieOk(req)) return send(res, 403, 'Open the address the runner printed (it ends in ?t=...).', 'text/plain; charset=utf-8');
    }

    if (req.method === 'GET' && Object.hasOwn(STATIC, url.pathname)) {
      const [file, type] = STATIC[url.pathname];
      let body = readFileSync(path.join(UI_DIR, file));
      if (file === 'index.html') body = body.toString('utf8').replace('__BYOS_TOKEN__', token);
      return send(res, 200, body, type);
    }
    if (!url.pathname.startsWith('/api/')) return send(res, 404, { error: 'not found' });
    if (!tokenOk(req, url)) return send(res, 403, { error: 'bad token' });
    const origin = req.headers.origin;
    if (origin && origin !== `http://127.0.0.1:${port}` && origin !== `http://localhost:${port}`) {
      return send(res, 403, { error: 'bad origin' });
    }

    const m = url.pathname.match(/^\/api\/jobs\/(\d+)(\/events|\/answer|\/cancel)?$/);
    try {
      if (req.method === 'GET' && url.pathname === '/api/info') {
        return send(res, 200, {
          cli: resolved.error ? null : { kind: resolved.kind, where: resolved.where },
          error: resolved.error ?? null,
          mock: !!options.mock,
          platform: process.platform,
        });
      }
      if (req.method === 'POST' && url.pathname === '/api/jobs') {
        const body = await readJson(req);
        if (body.command === 'put') throw new UsageError('use /api/upload');
        const job = startJob(String(body.command ?? ''), body.params ?? {});
        return send(res, 202, { id: job.id });
      }
      if (req.method === 'POST' && url.pathname === '/api/upload') {
        return await upload(req, res, url);
      }
      if (m) {
        const job = jobs.get(m[1]);
        if (!job) return send(res, 404, { error: 'no such job' });
        if (req.method === 'GET' && m[2] === '/events') return streamEvents(job, req, res);
        if (req.method === 'GET' && !m[2]) {
          return send(res, 200, { id: job.id, state: job.state, final: job.final, events: job.events });
        }
        if (req.method === 'POST' && m[2] === '/answer') {
          const body = await readJson(req);
          return send(res, job.answer(body.answer ?? null) ? 200 : 409, { ok: true });
        }
        if (req.method === 'POST' && m[2] === '/cancel') {
          if (job.state === 'queued') {
            dropQueued(job, 'Cancelled before it started');
            return send(res, 200, { ok: true });
          }
          return send(res, job.cancel() ? 200 : 409, { ok: true });
        }
      }
      return send(res, 404, { error: 'not found' });
    } catch (e) {
      if (e instanceof UsageError) return send(res, 400, { error: e.message });
      return send(res, e.status ?? 500, { error: e.message });
    }
  }

  // Server-sent events: everything so far, then live events, then "end".
  function streamEvents(job, req, res) {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      Connection: 'keep-alive',
      ...SECURITY_HEADERS,
    });
    const write = (ev) => res.write(`data: ${JSON.stringify(ev)}\n\n`);
    const end = () => {
      res.write(`event: end\ndata: ${JSON.stringify(job.final)}\n\n`);
      res.end();
    };
    res.write(`event: state\ndata: ${JSON.stringify({ state: job.state })}\n\n`);
    for (const ev of job.events) write(ev);
    if (job.state === 'done') return end();
    job.on('event', write);
    job.once('end', end);
    req.on('close', () => {
      job.off('event', write);
      job.off('end', end);
    });
  }

  // Receives one file from the browser into a private temporary folder, then runs
  // "byos-cli put" on it. The temporary copy is deleted when the job ends.
  async function upload(req, res, url) {
    const name = (url.searchParams.get('name') ?? '').normalize('NFC');
    const dir = url.searchParams.get('dir') ?? '';
    const device = url.searchParams.get('device') ?? undefined;
    const mode = url.searchParams.get('mode') ?? 'skip-identical';
    if (checkPath(name) || name.includes('/')) throw new UsageError(`invalid file name "${name}"`);
    const length = Number(req.headers['content-length']);
    if (!Number.isFinite(length) || length < 0 || length > MAX_UPLOAD) throw new UsageError('bad Content-Length');
    // The original file's modification time (File.lastModified, ms), so byos-cli sends that
    // and not the time of the temporary copy (music sync compares mtimes).
    let mtimeMs = null;
    if (url.searchParams.has('mtime')) {
      mtimeMs = Number(url.searchParams.get('mtime'));
      if (!Number.isSafeInteger(mtimeMs) || mtimeMs < 0 || mtimeMs > 4102444800000) {
        throw new UsageError('mtime: milliseconds since 1970, up to 2100');
      }
    }
    // Check everything before taking the body, so nothing is written for a refused request.
    if (resolved.error) throw Object.assign(new Error(resolved.error), { status: 503 });
    buildArgs('put', { local: [path.join(os.tmpdir(), 'x')], dir, device, mode }, globals);
    const tmp = mkdtempSync(path.join(os.tmpdir(), `byos-desktop-${process.pid}-`));
    const cleanup = () => rmSync(tmp, { recursive: true, force: true });
    const local = path.join(tmp, name);
    let job;
    try {
      await pipeline(req, createWriteStream(local, { flags: 'wx' }));
      if (mtimeMs !== null) utimesSync(local, new Date(), new Date(mtimeMs));
      job = startJob('put', { local: [local], dir, device, mode }, cleanup);
    } catch (e) {
      cleanup();
      throw e;
    }
    return send(res, 202, { id: job.id });
  }

  const server = http.createServer((req, res) => {
    handle(req, res).catch((e) => {
      if (!res.headersSent) send(res, 500, { error: e.message });
      else res.destroy();
    });
  });

  return {
    server,
    token,
    resolved,
    jobs,
    listen(p = 0, host = '127.0.0.1') {
      return new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(p, host, () => {
          port = server.address().port;
          resolve(`http://127.0.0.1:${port}/`);
        });
      });
    },
    // Stops every job and waits until each one has ended (and its temporary upload is gone),
    // at most until the kill grace period is over, then closes the server.
    async close() {
      const ending = [];
      for (const job of jobs.values()) {
        if (job.state === 'queued') dropQueued(job, 'The runner was closed');
        else if (job.state === 'running') {
          ending.push(new Promise((r) => job.once('end', r)));
          job.cancel('shutdown');
        }
      }
      const grace = (options.killGraceMs ?? 5000) + 2000;
      await Promise.race([Promise.all(ending), new Promise((r) => setTimeout(r, grace).unref())]);
      await new Promise((resolve) => {
        server.close(() => resolve());
        server.closeAllConnections?.(); // open event streams
      });
    },
  };
}

function parseArgs(argv) {
  const o = { port: 0 }; // a random free port unless --port is given
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--mock') o.mock = true;
    else if (a === '--open') o.open = true;
    else if (a === '--port') o.port = Number(argv[++i]);
    else if (a === '--cli') o.cli = argv[++i];
    else if (a === '--config') o.config = argv[++i];
    else if (a === '--adb') o.adb = argv[++i];
    else if (a === '--help' || a === '-h') o.help = true;
    else throw new Error(`unknown option ${a}`);
  }
  return o;
}

// Opens the UI in its own window when Microsoft Edge is there (app mode), otherwise in the
// default browser. This only opens a page on this computer.
function openWindow(url) {
  if (process.platform === 'win32') {
    const edge = path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'Microsoft', 'Edge', 'Application', 'msedge.exe');
    const child = spawn(edge, [`--app=${url}`, '--window-size=1180,760'], { detached: true, stdio: 'ignore' });
    child.on('error', () => spawn('cmd', ['/c', 'start', '', url], { detached: true, stdio: 'ignore', windowsHide: true }).unref());
    child.unref();
  } else {
    const opener = process.platform === 'darwin' ? 'open' : 'xdg-open';
    spawn(opener, [url], { detached: true, stdio: 'ignore' }).unref();
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  let o;
  try {
    o = parseArgs(process.argv.slice(2));
  } catch (e) {
    console.error(e.message);
    process.exit(1);
  }
  if (o.help) {
    console.log('node server.mjs [--mock] [--cli PATH] [--port N] [--open] [--config DIR] [--adb PATH]');
    process.exit(0);
  }
  const swept = sweepStaleTemp();
  if (swept) console.log(`Removed ${swept} temporary upload folder(s) left by an earlier run.`);
  const app = createApp(o);
  if (app.resolved.error) console.warn(`Warning: ${app.resolved.error}`);
  const base = await app.listen(o.port);
  // The address carries the token once; the page then keeps it in an HttpOnly cookie.
  const url = `${base}?t=${app.token}`;
  console.log(`ByOS desktop scaffold: ${url}  (byos-cli: ${app.resolved.kind ?? 'missing'})`);
  console.log('Keep this address private: it gives full access to the connected HiBy.');
  console.log('Press Ctrl+C to stop.');
  if (o.open) openWindow(url);
  let stopping = false;
  const stop = () => {
    if (stopping) return;
    stopping = true;
    app.close().then(() => process.exit(0));
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
}
