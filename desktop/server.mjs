#!/usr/bin/env node
// ByOS desktop scaffold: a tiny local runner. It serves the UI in ui/ on 127.0.0.1 and runs
// byos-cli for it (lib/cli.mjs). Node.js only, no packages. See README.md.
//
//   node server.mjs [--mock] [--cli PATH] [--port N] [--open] [--config DIR] [--adb PATH]

import { randomBytes, timingSafeEqual } from 'node:crypto';
import {
  copyFileSync, createWriteStream, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, utimesSync, writeFileSync,
} from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { buildArgs, cliConfigDir, DESKTOP_DIR, Job, resolveCli, UsageError } from './lib/cli.mjs';
import { checkPath } from './lib/btp-path.mjs';
import {
  cacheFile, DEVICE_ID, defaultIncomingDir, defaultSettingsDir, imageType, incomingTarget, isDir, SESSION, Settings,
} from './lib/media-store.mjs';

const UI_DIR = path.join(DESKTOP_DIR, 'ui');
const STATIC = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/i18n.js': ['i18n.js', 'text/javascript; charset=utf-8'],
  '/app.css': ['app.css', 'text/css; charset=utf-8'],
  '/icon.svg': ['icon.svg', 'image/svg+xml'],
  // The same path rules as the runner, shared with the browser.
  '/btp-path.js': ['../lib/btp-path.mjs', 'text/javascript; charset=utf-8'],
  // The Updates page's build-and-install steps, shared with the tests.
  '/build-flow.js': ['../lib/build-flow.mjs', 'text/javascript; charset=utf-8'],
  // The Library and Music tabs, and their formatting rules (shared with the tests).
  '/media.js': ['media.js', 'text/javascript; charset=utf-8'],
  '/media-format.js': ['../lib/media-format.mjs', 'text/javascript; charset=utf-8'],
  // The Sync tab (linked folders) and its rules (shared with the tests).
  '/sync.js': ['sync.js', 'text/javascript; charset=utf-8'],
  '/sync-flow.js': ['../lib/sync-flow.mjs', 'text/javascript; charset=utf-8'],
};
const MAX_INCOMING = 4 * 1024 ** 3 - 1;
const MAX_UPLOAD = 4 * 1024 ** 3 - 1; // FAT32 file limit
const MAX_FIRMWARE = 512 * 1024 ** 2; // HiBy's official firmware is about 47 MB
// Commands that need no HiBy: they never wait behind a device's queue. "build-info" and
// "bundle-update --check" answer at once; the long ones (download, build) share one queue.
const INSTANT = new Set(['devices', 'build-info']);
const LOCAL_QUEUE = '#local';
const LOCAL = new Set(['firmware-fetch', 'build', 'bundle-update', 'music-scan']);
// Link commands with --device-id need no HiBy: they wait behind each other only.
const LINKS_QUEUE = '#links';
const LINK_COMMANDS = new Set(['link-ls', 'link-add', 'link-set', 'link-rm']);
const JOB_KEEP_MS = 10 * 60 * 1000;
const MAX_BODY = 64 * 1024;
// A job request may carry the Sync preview's choices: one short entry per file of a large plan.
const MAX_JOB_BODY = 16 * 1024 * 1024;
const CHOICES = new Set(['delete', 'keep', 'pull', 'computer', 'player', 'both', 'skip']);

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

async function readJson(req, limit = MAX_BODY) {
  let size = 0;
  const chunks = [];
  for await (const c of req) {
    size += c.length;
    if (size > limit) throw new UsageError('request too large');
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

// The Sync preview's choices (DEC.json): item ids to a choice, and an optional policy per kind.
export function checkDecisions(d) {
  if (d === null || typeof d !== 'object' || Array.isArray(d)) throw new UsageError('decisions must be an object');
  const out = {};
  let n = 0;
  for (const [k, v] of Object.entries(d)) {
    if (k === 'policy') {
      if (v === null || typeof v !== 'object' || Array.isArray(v)) throw new UsageError('decisions.policy must be an object');
      const pol = {};
      for (const [kind, pv] of Object.entries(v)) {
        if (!['music', 'audiobooks', 'ebooks', 'video'].includes(kind) || !['keep', 'delete', 'ask'].includes(pv?.player_only)) {
          throw new UsageError('decisions.policy: {kind: {player_only: keep|delete|ask}}');
        }
        pol[kind] = { player_only: pv.player_only };
      }
      out.policy = pol;
      continue;
    }
    if (!/^[A-Za-z0-9_-]{1,64}$/.test(k) || !CHOICES.has(v)) throw new UsageError(`decisions: bad entry ${k.slice(0, 64)}`);
    if (++n > 500000) throw new UsageError('decisions: too many entries');
    out[k] = v;
  }
  return out;
}

export function createApp(options = {}) {
  const token = options.token ?? randomBytes(24).toString('hex');
  const resolved = resolveCli({ cli: options.cli, mock: options.mock, env: options.env });
  // The demo keeps its own byos-cli folder, so it never mixes with a real HiBy's cache.
  const config = options.config ?? (options.mock ? path.join(os.tmpdir(), 'byos-desktop-mock-config') : undefined);
  const globals = { config, adb: options.adb };
  const cache = path.join(config ?? cliConfigDir(options.env ?? process.env), 'cache');
  const settings = new Settings(options.settingsDir ?? (options.mock ? path.join(os.tmpdir(), 'byos-desktop-mock-settings') : defaultSettingsDir(options.env ?? process.env)));
  const incoming = options.incomingDir ?? defaultIncomingDir(options.env ?? process.env);
  const pickFolder = options.pickFolder ?? (options.mock ? demoFolder : windowsFolderPicker);
  const configDir = config ?? cliConfigDir(options.env ?? process.env);
  // Folders chosen in this window, by an opaque token: the page names a folder for link-add or
  // link-set only by its token, never by a path (design 10.2: the page never posts a path).
  const folders = new Map();
  const folderToken = (p) => {
    for (const [k, v] of folders) if (v === p) return k;
    const k = randomBytes(12).toString('hex');
    folders.set(k, p);
    return k;
  };
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
    job.spec = LOCAL.has(command) ? LOCAL_QUEUE
      : LINK_COMMANDS.has(command) && params.deviceId ? LINKS_QUEUE
        : typeof params.device === 'string' ? params.device : '';
    job.state = 'queued';
    if (cleanup) job.once('end', cleanup);
    // Every job, however it ends (run, cancelled while queued, shut down), is forgotten later.
    job.once('end', () => setTimeout(() => jobs.delete(job.id), options.jobKeepMs ?? JOB_KEEP_MS).unref());
    jobs.set(job.id, job);
    if (INSTANT.has(command)) {
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
        // Other query parameters (?lang=, ?demo=) are kept; the address fragment is kept by the browser.
        const rest = new URLSearchParams(url.searchParams);
        rest.delete('t');
        const query = rest.toString();
        res.writeHead(303, {
          Location: query ? `/?${query}` : '/',
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
        const body = await readJson(req, MAX_JOB_BODY);
        if (body.command === 'put') throw new UsageError('use /api/upload');
        if (body.command === 'firmware-fetch' && body.params?.from !== undefined) throw new UsageError('use /api/firmware');
        const command = String(body.command ?? '');
        const job = startJob(command, mediaParams(command, body.params ?? {}));
        return send(res, 202, { id: job.id });
      }
      // ---- Library and Music tabs: the files byos-cli made in its cache, the music folders
      const cm = url.pathname.match(/^\/api\/(library|art)\/([^/]+)$/);
      if (req.method === 'GET' && (cm || url.pathname === '/api/catalog' || url.pathname === '/api/plan')) {
        // /api/plan?check=1: the idle check's own plan (it never replaces the plan of the preview)
        const kind = cm ? cm[1] : url.pathname === '/api/plan' && url.searchParams.get('check') === '1' ? 'plan-check' : url.pathname.slice(5);
        const file = cacheFile(cache, kind, cm ? decodeURIComponent(cm[2]) : undefined);
        if (!file) return send(res, 400, { error: 'bad name' });
        let buf;
        try { buf = readFileSync(file); } catch { return send(res, 404, { error: 'none yet' }); }
        if (kind !== 'art') return send(res, 200, buf);
        const type = imageType(buf);
        if (!type) return send(res, 404, { error: 'not an image' });
        res.writeHead(200, { 'Content-Type': type, ...SECURITY_HEADERS, 'Cache-Control': 'private, max-age=604800' });
        return res.end(buf);
      }
      if (req.method === 'GET' && url.pathname === '/api/sources') return send(res, 200, { sources: settings.sources() });
      // ---- Sync tab: a folder for a link (by token), and the Music tab's folders to migrate
      if (req.method === 'POST' && url.pathname === '/api/links/pick') {
        // the kind only words the dialog ("Choose your music folder"); the folder is the user's choice
        const { kind } = await readJson(req);
        const picked = await pickFolder(FOLDER_KINDS[kind] ? kind : 'music');
        if (!picked) return send(res, 200, { folder: null });
        return send(res, 200, { folder: folderToken(picked), name: path.basename(picked) || picked, path: picked });
      }
      if (req.method === 'GET' && url.pathname === '/api/migration') {
        const id = url.searchParams.get('device') ?? '';
        if (!DEVICE_ID.test(id)) throw new UsageError('device: a device id');
        const done = (settings.read().migrated ?? []).includes(id);
        const sources = settings.sources().map((s) => ({ ...s, folder: s.exists ? folderToken(s.path) : null, name: path.basename(s.path) }));
        return send(res, 200, { migrated: done, sources });
      }
      if (req.method === 'POST' && url.pathname === '/api/migration/done') {
        const body = await readJson(req);
        if (!DEVICE_ID.test(body.device ?? '')) throw new UsageError('device: a device id');
        const v = settings.read();
        v.migrated = [...new Set([...(Array.isArray(v.migrated) ? v.migrated : []), body.device])];
        settings.write(v);
        return send(res, 200, { ok: true });
      }
      if (req.method === 'POST' && url.pathname === '/api/pick-folder') {
        const picked = await pickFolder();
        if (!picked) return send(res, 200, { picked: null, sources: settings.sources() });
        return send(res, 200, { picked, sources: settings.addSource(picked) });
      }
      if (req.method === 'DELETE' && url.pathname === '/api/sources') {
        const body = await readJson(req);
        const gone = settings.sources().find((s) => s.path === body.path);
        const list = settings.removeSource(body.path);
        // a folder of dropped files belongs to the runner: removed with its entry
        if (gone?.temp && gone.path.startsWith(incoming + path.sep)) rmSync(gone.path, { recursive: true, force: true });
        return send(res, 200, { sources: list });
      }
      if (req.method === 'POST' && url.pathname === '/api/incoming') return await incomingFile(req, res, url);
      if (req.method === 'POST' && url.pathname === '/api/incoming/done') {
        const session = url.searchParams.get('session') ?? '';
        if (!SESSION.test(session)) throw new UsageError('bad session');
        const dir = path.join(incoming, session);
        if (!isDir(dir)) throw new UsageError('nothing was dropped');
        return send(res, 200, { sources: settings.addSource(dir, true) });
      }
      if (req.method === 'POST' && url.pathname === '/api/upload') {
        return await upload(req, res, url);
      }
      if (req.method === 'POST' && url.pathname === '/api/firmware') {
        return await firmwareUpload(req, res);
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
      if (e instanceof UsageError) return send(res, e.code === 'plan_changed' ? 409 : 400, e.code ? { error: e.message, code: e.code } : { error: e.message });
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

  // The page never names files on this computer: music folders come from the links (or the
  // settings), a link's folder from a picked token, the plan and decisions live in the cache.
  function mediaParams(command, p) {
    if (p === null || typeof p !== 'object' || Array.isArray(p)) throw new UsageError('params must be an object');
    const { catalog: _c, plan: _p, sources: _s, local: _l, out: _o, decisions, folder, check, runId, ...rest } = p;
    switch (command) {
      case 'music-scan': {
        const dirs = musicFolders();
        if (!dirs.length) throw new UsageError('choose a music folder first');
        return { ...rest, sources: dirs };
      }
      case 'link-add':
      case 'link-set': {
        const out = { ...rest };
        if (folder !== undefined && folder !== null) {
          const dir = typeof folder === 'string' ? folders.get(folder) : undefined;
          if (!dir) throw new UsageError('folder: choose the folder again');
          out.local = dir;
        } else if (command === 'link-add') throw new UsageError('link-add: choose a folder first');
        return out;
      }
      case 'sync-plan': {
        // linked folders; the plan goes to the runner's cache, where /api/plan reads it. The Sync
        // tab's quiet idle check ({check: true}) writes its own file: only the plan the user opened
        // in the preview is the one sync-apply reads.
        const { select: _x, selection: _y, dest: _d, layout: _la, covers: _co, removeUnselected: _r, ...keep } = rest;
        const out = { ...keep, out: cacheFile(cache, check === true ? 'plan-check' : 'plan') };
        mkdirSync(path.dirname(out.out), { recursive: true });
        return out;
      }
      case 'sync-apply': {
        // The choices name items of one plan (i1, i2… are numbered per plan): they are only sent
        // with the plan they were made on. The page names it by its run_id; the plan is copied
        // first, so a later sync-plan cannot change it under byos-cli.
        const hasChoices = decisions !== undefined && decisions !== null && Object.keys(decisions).some((k) => k !== 'policy');
        if (hasChoices && runId === undefined) throw new UsageError('sync-apply: runId (the plan the choices were made on) is required with decisions');
        let planFile = cacheFile(cache, 'plan');
        if (runId !== undefined) {
          if (typeof runId !== 'string' || !/^[A-Za-z0-9_.:-]{1,80}$/.test(runId)) throw new UsageError('runId: the run_id of the plan');
          let cur = null;
          try { cur = JSON.parse(readFileSync(planFile, 'utf8'))?.run_id ?? null; } catch { /* no plan */ }
          if (cur !== runId) {
            throw Object.assign(new UsageError('the changes were looked up again since this preview was made; review them again'), { code: 'plan_changed' });
          }
          const copy = cacheFile(cache, 'plan-apply');
          mkdirSync(path.dirname(copy), { recursive: true });
          copyFileSync(planFile, copy);
          planFile = copy;
        }
        const out = { ...rest, linked: true, plan: planFile };
        if (decisions !== undefined) {
          const f = path.join(cache, 'sync', 'decisions.json');
          mkdirSync(path.dirname(f), { recursive: true });
          writeFileSync(f, JSON.stringify(checkDecisions(decisions)));
          out.decisions = f;
        }
        return out;
      }
      default:
        return rest;
    }
  }

  // The music and audiobook folders of every HiBy's links (byos-cli's links.json files), for the
  // computer's library in the sidebar; before any link, the Music tab's folders.
  function musicFolders() {
    const dirs = [];
    const seen = new Set();
    const add = (p) => {
      if (typeof p !== 'string' || !path.isAbsolute(p) || !isDir(p)) return;
      const k = path.resolve(p).toLowerCase();
      if (!seen.has(k)) { seen.add(k); dirs.push(p); }
    };
    let devs = [];
    try { devs = readdirSync(path.join(configDir, 'sync')); } catch { /* no links yet */ }
    for (const d of devs) {
      if (!DEVICE_ID.test(d)) continue;
      let lf;
      try { lf = JSON.parse(readFileSync(path.join(configDir, 'sync', d, 'links.json'), 'utf8')); } catch { continue; }
      for (const l of Array.isArray(lf?.links) ? lf.links : []) {
        if (!l || l.enabled === false || !['music', 'audiobooks'].includes(l.kind)) continue;
        add(typeof l.local === 'string' ? l.local : l.local?.path);
      }
    }
    if (!dirs.length) for (const s of settings.sources()) if (s.exists) add(s.path);
    return dirs.slice(0, 32);
  }

  // One file dropped on the Music tab, streamed into the drop's own folder under "incoming"
  // (the browser has no paths: the folder then becomes a temporary music source).
  async function incomingFile(req, res, url) {
    const target = incomingTarget(incoming, url.searchParams.get('session'), url.searchParams.get('rel'));
    if (!target) throw new UsageError('bad session or name');
    const length = Number(req.headers['content-length']);
    if (!Number.isFinite(length) || length < 0 || length > MAX_INCOMING) throw new UsageError('bad Content-Length');
    mkdirSync(path.dirname(target.full), { recursive: true });
    try {
      await pipeline(req, createWriteStream(target.full, { flags: 'w' }));
      if (url.searchParams.has('mtime')) {
        const ms = Number(url.searchParams.get('mtime'));
        if (Number.isSafeInteger(ms) && ms > 0 && ms < 4102444800000) utimesSync(target.full, new Date(), new Date(ms));
      }
    } catch (e) {
      rmSync(target.full, { force: true });
      throw e;
    }
    return send(res, 200, { ok: true });
  }

  // Receives an official firmware file the user picked into a private temporary folder, then
  // runs "byos-cli firmware-fetch --from" on it: byos-cli checks it (size, MD5, SHA-256) and
  // keeps its own copy. The temporary copy is deleted when the job ends.
  async function firmwareUpload(req, res) {
    const length = Number(req.headers['content-length']);
    if (!Number.isFinite(length) || length <= 0 || length > MAX_FIRMWARE) throw new UsageError('bad Content-Length');
    if (resolved.error) throw Object.assign(new Error(resolved.error), { status: 503 });
    const tmp = mkdtempSync(path.join(os.tmpdir(), `byos-desktop-${process.pid}-`));
    const cleanup = () => rmSync(tmp, { recursive: true, force: true });
    const local = path.join(tmp, 'firmware.upt');
    let job;
    try {
      await pipeline(req, createWriteStream(local, { flags: 'wx' }));
      job = startJob('firmware-fetch', { from: local }, cleanup);
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
    cache,
    settings,
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

const FOLDER_KINDS = {
  music: ['Music', 'Choose your music folder for ByOS'],
  audiobooks: ['Audiobooks', 'Choose your audiobooks folder for ByOS'],
  ebooks: ['Books', 'Choose your books folder for ByOS'],
  video: ['Videos', 'Choose your video folder for ByOS'],
};

// Windows' own folder dialog, through PowerShell (no extra package). Resolves the chosen
// folder, or null when the user closed the dialog.
function windowsFolderPicker(kind = 'music') {
  if (process.platform !== 'win32') return Promise.resolve(null);
  const script = [
    'Add-Type -AssemblyName System.Windows.Forms',
    '[Console]::OutputEncoding = [Text.Encoding]::UTF8',
    '$owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }',
    '$d = New-Object System.Windows.Forms.FolderBrowserDialog',
    `$d.Description = "${FOLDER_KINDS[kind]?.[1] ?? FOLDER_KINDS.music[1]}"`,
    '$d.ShowNewFolderButton = $false',
    'if ($d.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $d.SelectedPath }',
  ].join('; ');
  return new Promise((resolve) => {
    const child = spawn('powershell.exe', ['-NoProfile', '-STA', '-NonInteractive', '-Command', script], { windowsHide: true });
    let out = '';
    child.stdout.on('data', (c) => { out += c.toString('utf8'); });
    child.on('error', () => resolve(null));
    child.on('close', () => {
      const p = out.trim().split(/\r?\n/).pop()?.trim();
      resolve(p && path.isAbsolute(p) && isDir(p) ? p : null);
    });
  });
}

// The demo's "folder picker": a folder in the temporary folder (the fake byos-cli invents its music).
function demoFolder(kind = 'music') {
  const d = path.join(os.tmpdir(), 'byos-demo-music', FOLDER_KINDS[kind]?.[0] ?? 'Music');
  mkdirSync(d, { recursive: true });
  return Promise.resolve(d);
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
