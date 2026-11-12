// Runs byos-cli (docs/BYOS_CLI.md) as a child process and turns its JSON-lines output into
// job events. One command per process, arguments as an array (never through a shell).

import { spawn, spawnSync } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { existsSync, statSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { LineSplitter, parseEventLine, TERMINAL } from './jsonl.mjs';
import { checkPath, normalise } from './btp-path.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
export const DESKTOP_DIR = path.resolve(here, '..');
export const REPO_DIR = path.resolve(DESKTOP_DIR, '..');
export const MOCK_CLI = path.join(DESKTOP_DIR, 'mock', 'byos-cli-mock.mjs');
export const DEFAULT_JAR = path.join(
  REPO_DIR, 'android', 'hibyos-install', 'cli', 'build', 'libs', 'byos-cli.jar');

// Finds how to start byos-cli. Order: explicit option, BYOS_CLI environment variable, the
// jar built by the :cli Gradle module. With mock: the fake CLI in mock/.
// Returns { program, prefix, kind, where } or { error }.
export function resolveCli({ cli, mock = false, env = process.env } = {}) {
  if (mock) {
    return { program: process.execPath, prefix: [MOCK_CLI], kind: 'mock', where: MOCK_CLI };
  }
  const target = cli || env.BYOS_CLI || DEFAULT_JAR;
  if (!existsSync(target)) {
    return {
      error: `byos-cli not found at ${target}. Build it with "gradlew :cli:fatJar" in ` +
        'android/hibyos-install, set BYOS_CLI, or start with --mock.',
    };
  }
  if (target.toLowerCase().endsWith('.jar')) {
    const java = env.JAVA_HOME ? path.join(env.JAVA_HOME, 'bin', 'java') : 'java';
    return { program: java, prefix: ['-jar', target], kind: 'jar', where: target };
  }
  if (target.toLowerCase().endsWith('.mjs') || target.toLowerCase().endsWith('.js')) {
    return { program: process.execPath, prefix: [target], kind: 'node', where: target };
  }
  return { program: target, prefix: [], kind: 'native', where: target };
}

export class UsageError extends Error {}

// A USB serial may contain spaces (the HiBy R3 II 2025 reports "HiBy R3II_2025"): safe, the
// arguments go to the process as a list, never through a shell.
const DEVICE_SPEC = /^(usb|wifi|usb:[A-Za-z0-9._:-](?:[A-Za-z0-9._: -]{0,62}[A-Za-z0-9._:-])?|wifi:[0-9a-f]{4,16}|[A-Za-z0-9][A-Za-z0-9.-]{0,252}(:\d{1,5})?)$/;

function remote(p, what, { allowEmpty = false } = {}) {
  if (typeof p !== 'string') throw new UsageError(`${what}: a path is required`);
  const n = normalise(p);
  const reason = checkPath(n, { allowEmpty });
  if (reason) throw new UsageError(`${what}: invalid path "${p}" (${reason})`);
  return n;
}

// Paths always go after a "--" end-of-options marker (BYOS_CLI section 2), so a name such as
// "-r" or "--yes" stays a path. Flags come first.
function withPaths(head, flags, paths) {
  return paths.length ? [...head, ...flags, '--', ...paths] : [...head, ...flags];
}

// A playlist name as the page types it: one card-safe name, no folders.
function playlistName(n) {
  if (typeof n !== 'string') throw new UsageError('playlist: a name is required');
  const s = n.normalize('NFC').trim();
  if (!s || s.length > 120 || s.includes('/') || checkPath(s)) throw new UsageError(`playlist: invalid name "${n}"`);
  return s;
}

// An existing playlist by its card path ("Playlists/Road Trip.m3u"): .m3u and .m3u8 files.
function playlistFile(p) {
  const n = remote(p, 'playlist');
  if (!/\.m3u8?$/i.test(n)) throw new UsageError(`playlist: "${p}" is not an .m3u or .m3u8 file`);
  return n;
}

function oneOf(v, allowed, what) {
  if (!allowed.includes(v)) throw new UsageError(`${what}: one of ${allowed.join(', ')}`);
  return v;
}

// A folder on this computer: absolute and existing (it comes from the runner's settings).
function localDir(p, what) {
  if (typeof p !== 'string' || !path.isAbsolute(p)) throw new UsageError(`${what}: folders must be absolute paths`);
  let st;
  try { st = statSync(p); } catch { throw new UsageError(`${what}: ${p} does not exist`); }
  if (!st.isDirectory()) throw new UsageError(`${what}: ${p} is not a folder`);
  return p;
}

// A file the runner made in its cache (catalogue, selection, plan).
function localFile(p, what) {
  if (typeof p !== 'string' || !path.isAbsolute(p)) throw new UsageError(`${what}: the runner's file must be an absolute path`);
  return p;
}

const KINDS = ['music', 'audiobooks', 'ebooks', 'video'];

// A link id as byos-cli makes them (two random hex ids).
function linkId(id) {
  if (typeof id !== 'string' || !/^[0-9a-f]{4,64}$/.test(id)) throw new UsageError('link: invalid link id');
  return id;
}

// --device-id: the HiBy the links belong to, so link commands work without the HiBy.
function deviceIdFlag(params) {
  if (params.deviceId === undefined || params.deviceId === null) return [];
  if (typeof params.deviceId !== 'string' || !/^[0-9a-f]{4,64}$/.test(params.deviceId)) throw new UsageError('deviceId: a hex device id');
  return ['--device-id', params.deviceId];
}

function remoteList(list, what) {
  if (!Array.isArray(list) || list.length === 0 || list.length > 1000) {
    throw new UsageError(`${what}: 1 to 1000 paths are required`);
  }
  return list.map((p) => remote(p, what));
}

// Builds the byos-cli argument list for one UI request. Only these commands can be started
// from the UI, and every parameter is checked here.
export function buildArgs(command, params = {}, globals = {}) {
  const g = [];
  if (globals.config) g.push('--config', globals.config);
  if (globals.adb) g.push('--adb', globals.adb);
  if (params.device !== undefined && params.device !== null && params.device !== '') {
    if (typeof params.device !== 'string' || !DEVICE_SPEC.test(params.device)) {
      throw new UsageError('device: invalid device spec');
    }
    g.push('--device', params.device);
  }
  const a = [];
  switch (command) {
    case 'devices': {
      const wait = params.wait ?? 1500;
      if (!Number.isInteger(wait) || wait < 0 || wait > 10000) throw new UsageError('wait: 0-10000');
      a.push('devices', '--wait', String(wait));
      if (params.usbOnly) a.push('--usb-only');
      break;
    }
    case 'status':
    case 'df':
      a.push(command);
      if (params.start) a.push('--start');
      break;
    case 'ls': {
      // The SD root is the command without a path (BYOS_CLI: "" or omitted = SD root).
      const p = remote(params.path ?? '', 'ls', { allowEmpty: true });
      a.push(...withPaths(['ls'], params.all ? ['--all'] : [], p ? [p] : []));
      break;
    }
    case 'mkdir':
      a.push(...withPaths(['mkdir'], params.parents ? ['-p'] : [], remoteList(params.paths, 'mkdir')));
      break;
    case 'mv':
      a.push(...withPaths(['mv'], params.replace ? ['--replace'] : [],
        [remote(params.src, 'mv'), remote(params.dst, 'mv')]));
      break;
    case 'rm': {
      const flags = [];
      if (params.recursive) flags.push('-r');
      if (params.yes) flags.push('--yes');
      if (params.pruneEmpty) flags.push('--prune-empty');
      a.push(...withPaths(['rm'], flags, remoteList(params.paths, 'rm')));
      break;
    }
    // ---- Library and Music tabs (BYOS_CLI.md, "Library and music"). Files on this computer
    // (catalogue, selection, plan, source folders) never come from the page: the runner
    // (server.mjs) fills them in from its own cache and settings.
    case 'lib-index':
      a.push('lib-index');
      break;
    case 'lib-scan': {
      const roots = params.roots === undefined ? [] : remoteList(params.roots, 'lib-scan');
      a.push(...withPaths(['lib-scan'], params.full ? ['--full'] : [], roots));
      break;
    }
    case 'lib-art': {
      if (!Array.isArray(params.paths) || params.paths.length === 0 || params.paths.length > 64) {
        throw new UsageError('lib-art: 1 to 64 paths are required');
      }
      const size = params.size ?? 256;
      if (size !== 256 && size !== 500) throw new UsageError('lib-art: size is 256 or 500');
      a.push(...withPaths(['lib-art'], ['--size', String(size)], params.paths.map((p) => remote(p, 'lib-art'))));
      break;
    }
    case 'lib-rescan':
      if (params.full) a.push('lib-rescan', '--full');
      else {
        if (!Array.isArray(params.paths) || params.paths.length === 0 || params.paths.length > 256) {
          throw new UsageError('lib-rescan: --full or 1 to 256 paths');
        }
        a.push(...withPaths(['lib-rescan'], [], params.paths.map((p) => remote(p, 'lib-rescan'))));
      }
      break;
    case 'playlist-ls':
      a.push('playlist-ls');
      break;
    case 'playlist-add':
    case 'playlist-rm': {
      const name = params.path !== undefined ? playlistFile(params.path) : playlistName(params.name);
      const paths = command === 'playlist-rm' && (params.paths === undefined || params.paths.length === 0)
        ? [] : remoteList(params.paths, command);
      const flags = command === 'playlist-add' && params.create ? ['--create'] : [];
      a.push(...withPaths([command], flags, [name, ...paths]));
      break;
    }
    case 'music-scan': {
      if (!Array.isArray(params.sources) || params.sources.length === 0 || params.sources.length > 32) {
        throw new UsageError('music-scan: 1 to 32 music folders are required');
      }
      a.push(...withPaths(['music-scan'], params.full ? ['--full'] : [], params.sources.map((s) => localDir(s, 'music-scan'))));
      break;
    }
    // ---- Sync tab: linked folders (BYOS_CLI.md "Folder sync"). The folder of a link-add or
    // link-set --local, the plan and the decisions file are filled in by the runner.
    case 'link-ls':
      a.push('link-ls', ...deviceIdFlag(params));
      break;
    case 'link-add':
    case 'link-set': {
      const head = [command];
      const flags = [...deviceIdFlag(params)];
      if (command === 'link-add') flags.push('--kind', oneOf(params.kind, KINDS, 'kind'));
      if (params.cardRoot !== undefined) flags.push('--card-root', remote(params.cardRoot, 'card root'));
      if (params.layout !== undefined) flags.push('--layout', oneOf(params.layout, ['artist-album', 'keep'], 'layout'));
      if (params.playerOnly !== undefined) flags.push('--player-only', oneOf(params.playerOnly, ['keep', 'delete', 'ask'], 'player-only'));
      if (params.covers !== undefined) flags.push('--covers', oneOf(params.covers, ['optimize', 'keep'], 'covers'));
      if (params.select !== undefined) {
        if (!Array.isArray(params.select) || params.select.length > 5000) throw new UsageError('select: up to 5000 values');
        for (const s of params.select) {
          if (typeof s !== 'string' || !/^(artist|album|genre):./s.test(s) || s.length > 600 || /[\u0000-\u001f]/.test(s)) {
            throw new UsageError('select: artist:NAME, album:ARTIST::TITLE or genre:NAME');
          }
          flags.push('--select', s);
        }
      }
      if (params.selectAll) flags.push('--select-all');
      if (params.removeUnselected === true) flags.push('--remove-unselected');
      if (params.removeUnselected === false && command === 'link-set') flags.push('--keep-unselected');
      if (params.enabled === false) flags.push('--disable');
      if (params.enabled === true && command === 'link-set') flags.push('--enable');
      if (params.label !== undefined) {
        if (typeof params.label !== 'string' || !params.label.trim() || params.label.length > 120 || /[\u0000-\u001f]/.test(params.label)) throw new UsageError('label: a short name');
        flags.push('--label', params.label.trim());
      }
      if (command === 'link-add') {
        a.push(...withPaths(head, flags, [localDir(params.local, 'link-add')]));
      } else {
        if (params.local !== undefined) flags.push('--local', localDir(params.local, 'link-set'));
        a.push(...withPaths(head, flags, [linkId(params.id)]));
      }
      break;
    }
    case 'link-rm':
      a.push(...withPaths(['link-rm'], deviceIdFlag(params), [linkId(params.id)]));
      break;
    case 'sync-scan':
      a.push('sync-scan', '--kind', oneOf(params.kind ?? 'all', [...KINDS, 'all'], 'kind'), ...deviceIdFlag(params));
      if (params.full) a.push('--full');
      break;
    case 'sync-status':
      a.push('sync-status', ...deviceIdFlag(params));
      break;
    case 'sync-unlock':
      a.push('sync-unlock');
      if (params.force) a.push('--force');
      break;
    case 'sync-resume':
      a.push('sync-resume', '--then', oneOf(params.then ?? 'none', ['none', 'rescan', 'rebuild'], 'then'));
      if (params.takeOver) a.push('--take-over');
      break;
    case 'sync-plan': {
      if (params.catalog === undefined) {
        // linked folders
        const flags = ['--kind', oneOf(params.kind ?? 'all', [...KINDS, 'all'], 'kind')];
        if (params.deleteUntracked) flags.push('--delete-untracked');
        if (params.noScan) flags.push('--no-scan');
        if (params.out !== undefined) flags.push('--out', localFile(params.out, 'sync-plan'));
        a.push('sync-plan', ...flags);
        break;
      }
      const flags = ['--catalog', localFile(params.catalog, 'sync-plan'), '--dest', remote(params.dest ?? 'Music', 'sync-plan destination')];
      if (params.select !== undefined) flags.push('--select', localFile(params.select, 'sync-plan'));
      flags.push('--layout', oneOf(params.layout ?? 'artist-album', ['artist-album', 'keep'], 'layout'));
      flags.push('--covers', oneOf(params.covers ?? 'optimize', ['optimize', 'keep'], 'covers'));
      if (params.removeUnselected) flags.push('--remove-unselected');
      a.push('sync-plan', ...flags);
      break;
    }
    case 'sync-apply': {
      if (params.linked) {
        const flags = ['--then', oneOf(params.then ?? 'none', ['none', 'rescan', 'rebuild'], 'then')];
        if (params.decisions !== undefined) flags.push('--decisions', localFile(params.decisions, 'sync-apply'));
        if (params.yes) flags.push('--yes');
        if (params.allowMassDelete) flags.push('--allow-mass-delete');
        if (params.takeOver) flags.push('--take-over');
        a.push(...withPaths(['sync-apply'], flags, [localFile(params.plan, 'sync-apply')]));
        break;
      }
      const flags = ['--covers', oneOf(params.covers ?? 'optimize', ['optimize', 'keep'], 'covers'),
        '--then', oneOf(params.then ?? 'none', ['none', 'rescan', 'rebuild'], 'then')];
      if (params.lyrics) flags.push('--lyrics');
      if (params.yes) flags.push('--yes');
      a.push(...withPaths(['sync-apply'], flags, [localFile(params.plan, 'sync-apply')]));
      break;
    }
    case 'cover-cache-reset':
      a.push('cover-cache-reset');
      break;
    case 'art-fix': {
      a.push('art-fix', '--root', remote(params.root ?? 'Music', 'art-fix'));
      if (params.dryRun) a.push('--dry-run');
      break;
    }
    case 'connect':
      a.push('connect');
      if (params.pair) a.push('--pair');
      if (params.code !== undefined) {
        if (!/^\d{6}$/.test(String(params.code))) throw new UsageError('code: 6 digits');
        a.push('--code', String(params.code));
      }
      if (params.forget) a.push('--forget');
      break;
    case 'put': {
      // Local files are only ever the runner's own temporary uploads (server.mjs).
      if (!Array.isArray(params.local) || params.local.length === 0) {
        throw new UsageError('put: local files are required');
      }
      if (params.local.some((l) => typeof l !== 'string' || !path.isAbsolute(l))) {
        throw new UsageError('put: local files must be absolute paths');
      }
      // The UI uploads into a folder only. The SD root holds the firmware files that the
      // recovery loader flashes (DESKTOP_APP.md 5.7), so the Files page never writes there.
      const dir = remote(params.dir, 'put (choose a folder, not the card root)');
      const flags = ['--mkdir'];
      if (params.mode === 'overwrite') flags.push('--overwrite');
      else if (params.mode !== 'create') flags.push('--skip-identical');
      a.push(...withPaths(['put'], flags, [...params.local, `${dir}/`]));
      break;
    }
    // ---- Build ByOS on this computer (BYOS_CLI.md, "Build ByOS"). No paths come from the
    // page: byos-cli keeps the firmware and the build in its own cache folder.
    case 'build-info':
      a.push('build-info');
      break;
    case 'bundle-update':
      a.push('bundle-update');
      if (params.check) a.push('--check');
      break;
    case 'firmware-fetch':
      a.push('firmware-fetch');
      // A firmware file the user picked: only ever the runner's own temporary copy (server.mjs).
      if (params.from !== undefined) {
        if (typeof params.from !== 'string' || !path.isAbsolute(params.from)) {
          throw new UsageError('firmware-fetch: the picked file must be an absolute path');
        }
        a.push('--from', params.from);
      }
      break;
    case 'build':
      a.push('build');
      if (params.force) a.push('--force');
      break;
    case 'build-send':
      // Firmware files never travel over Wi-Fi (DESKTOP_APP.md 5.7): refused here too.
      if (typeof params.device === 'string' && !/^usb(:|$)/.test(params.device)) {
        throw new UsageError('build-send: the HiBy must be connected with the USB cable');
      }
      a.push('build-send');
      if (params.install) a.push('--install');
      if (params.yes) a.push('--yes');
      if (params.noWait) a.push('--no-wait');
      break;
    default:
      throw new UsageError(`command not allowed from the UI: ${command}`);
  }
  return [...g, ...a];
}

let nextId = 1;

// How long a job may stay silent. byos-cli repeats its progress line at least every 5 s while
// it waits (BYOS_CLI section 2, heartbeat), so 25 s of silence normally means it is stuck.
// Some waits are long by design, so after a progress line with one of these "op" values the
// limit is longer, in case heartbeats are missing:
//   pair_wait           the user has up to 120 s to tap Allow (SYNC_PROTOCOL section 17)
//   install, reconnect  update-install reconnects for up to 60 s across the shell restart
export const DEFAULT_TIMEOUT_MS = 25000;
export const OP_TIMEOUTS_MS = Object.freeze({ pair_wait: 130000, install: 70000, reconnect: 70000 });
// A prompt (pairing code, confirmation) left unanswered this long is answered "no" and the
// job is cancelled. 130 s covers the device's own 120 s pairing limit.
export const PROMPT_TIMEOUT_MS = 130000;

// One byos-cli run. Emits 'event' (each checked JSON line, plus synthetic ones) and 'end'
// (the final result or error line). The 'events' array keeps everything for late readers.
export class Job extends EventEmitter {
  constructor(resolved, args, {
    timeoutMs = DEFAULT_TIMEOUT_MS, opTimeoutsMs = OP_TIMEOUTS_MS,
    promptTimeoutMs = PROMPT_TIMEOUT_MS, killGraceMs = 5000, env,
  } = {}) {
    super();
    this.id = String(nextId++);
    this.resolved = resolved;
    this.args = args;
    this.events = [];
    this.stderr = '';
    this.state = 'new';
    this.final = null;
    this.timeoutMs = timeoutMs;
    this.opTimeoutsMs = opTimeoutsMs ?? {};
    this.promptTimeoutMs = promptTimeoutMs;
    this.lastOp = null;
    this.killGraceMs = killGraceMs;
    this.env = env;
  }

  start() {
    const { program, prefix } = this.resolved;
    this.state = 'running';
    this.started = Date.now();
    this.child = spawn(program, [...prefix, ...this.args], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
      shell: false,
      env: this.env ?? process.env,
    });
    const lines = new LineSplitter((line) => this._line(line));
    this.child.stdout.on('data', (c) => lines.push(c));
    this.child.stdout.on('end', () => lines.end());
    this.child.stderr.on('data', (c) => {
      this.stderr = (this.stderr + c.toString('utf8')).slice(-8192);
    });
    this.child.stdin.on('error', () => {}); // the process may exit before reading stdin
    this.child.on('error', (err) => {
      this._finish({
        v: 1, event: 'error', t: this._t(), ok: false, code: 'no_cli',
        message: `Cannot start byos-cli: ${err.message}`,
      });
    });
    this.child.on('close', (code) => {
      clearTimeout(this.watchdog);
      clearTimeout(this.killTimer);
      if (!this.final) {
        this._finish({
          v: 1, event: 'error', t: this._t(), ok: false, code: 'internal',
          message: `byos-cli exited (code ${code}) without a result line`,
          stderr: this.stderr.split('\n').slice(-10).join('\n'),
        });
      }
      this.exitCode = code;
      this.state = 'done';
      this.emit('end', this.final);
    });
    this._arm();
    return this;
  }

  _t() {
    return Date.now() - (this.started ?? Date.now());
  }

  // The silence limit right now: the prompt limit while a prompt waits for the user, longer
  // after a progress line for a long wait, otherwise timeoutMs.
  currentLimit() {
    if (this.waitingForAnswer) return this.promptTimeoutMs || 0;
    if (!this.timeoutMs) return 0;
    const forOp = this.lastOp ? this.opTimeoutsMs[this.lastOp] ?? 0 : 0;
    return Math.max(this.timeoutMs, forOp);
  }

  _arm() {
    clearTimeout(this.watchdog);
    if (this.final || this.cancelReason || this.state !== 'running') return;
    const limit = this.currentLimit();
    if (!limit) return;
    this.watchdog = setTimeout(() => {
      if (this.final) return;
      if (this.waitingForAnswer) {
        this._record({ v: 1, event: 'warn', t: this._t(), code: 'prompt_timeout', message: 'No answer was given in time' });
        // "No" first (BYOS_CLI 2.2: an empty answer means no), then cancel.
        this.waitingForAnswer = false;
        try { this.child.stdin.write(`${JSON.stringify({ answer: null })}\n`); } catch { /* closed */ }
        this.cancel('prompt_timeout');
        return;
      }
      this._record({ v: 1, event: 'warn', t: this._t(), code: 'watchdog', message: 'byos-cli stopped reporting' });
      this.cancel('timeout');
    }, limit);
  }

  _line(line) {
    const r = parseEventLine(line);
    if (!r.ok) {
      this._record({ v: 1, event: 'warn', t: this._t(), code: 'bad_output', message: r.reason });
      return;
    }
    const ev = r.event;
    if (this.final) {
      this._record({ v: 1, event: 'warn', t: this._t(), code: 'bad_output', message: 'output after the final line' });
      return;
    }
    if (ev.event === 'prompt') this.waitingForAnswer = true;
    if (ev.event === 'progress' && typeof ev.op === 'string') this.lastOp = ev.op;
    if (TERMINAL.has(ev.event)) this._finish(ev);
    else this._record(ev);
    this._arm();
  }

  _record(ev) {
    this.events.push(ev);
    this.emit('event', ev);
  }

  _finish(ev) {
    if (this.final) return;
    if (this.cancelReason === 'timeout' && ev.event === 'error' && ev.code === 'cancelled') {
      ev = { ...ev, code: 'timeout', message: 'The HiBy stopped answering.' };
    }
    // Whatever the CLI made of the forced "no" (cancelled, auth, rejected), the cause is that
    // nobody answered.
    if (this.cancelReason === 'prompt_timeout' && ev.event === 'error') {
      ev = { ...ev, code: 'timeout', reason: 'prompt', message: 'No answer was given in time.' };
    }
    this.final = ev;
    this._record(ev);
    try { this.child?.stdin.end(); } catch { /* already closed */ }
  }

  // Answers the last prompt (BYOS_CLI 2.2).
  answer(value) {
    if (this.state !== 'running' || this.final) return false;
    this.waitingForAnswer = false;
    this.child.stdin.write(`${JSON.stringify({ answer: value })}\n`);
    this._arm();
    return true;
  }

  // Asks byos-cli to stop; kills it if it is still running after the grace period.
  cancel(reason = 'user') {
    if (this.state !== 'running' || this.final) return false;
    if (this.cancelReason) return true; // already stopping
    this.cancelReason = reason;
    clearTimeout(this.watchdog);
    try { this.child.stdin.write('{"cancel":true}\n'); } catch { /* closed */ }
    this.killTimer = setTimeout(() => this.kill(), this.killGraceMs);
    return true;
  }

  kill() {
    if (!this.child || this.child.exitCode !== null) return;
    if (process.platform === 'win32' && this.child.pid) {
      spawnSync('taskkill', ['/PID', String(this.child.pid), '/T', '/F'], { windowsHide: true });
    } else {
      this.child.kill('SIGKILL');
    }
    if (!this.final) {
      this._finish({
        v: 1, event: 'error', t: this._t(), ok: false,
        code: ['timeout', 'prompt_timeout'].includes(this.cancelReason) ? 'timeout' : 'cancelled',
        message: 'byos-cli was stopped',
      });
    }
  }
}

// byos-cli's own configuration folder (BYOS_CLI.md 3.1); its cache is "<folder>/cache".
export function cliConfigDir(env = process.env, platform = process.platform, home = os.homedir()) {
  if (platform === 'win32') return path.join(env.APPDATA || path.join(home, 'AppData', 'Roaming'), 'ByOS', 'cli');
  if (platform === 'darwin') return path.join(home, 'Library', 'Application Support', 'ByOS', 'cli');
  return env.XDG_CONFIG_HOME ? path.join(env.XDG_CONFIG_HOME, 'byos-cli') : path.join(home, '.config', 'byos-cli');
}

// Runs one job to the end and returns { final, events }. For tests and simple calls.
export function runJob(resolved, args, opts) {
  return new Promise((resolve) => {
    const job = new Job(resolved, args, opts);
    job.on('end', (final) => resolve({ final, events: job.events, exitCode: job.exitCode }));
    job.start();
  });
}
