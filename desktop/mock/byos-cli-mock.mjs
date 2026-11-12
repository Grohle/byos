#!/usr/bin/env node
// A fake byos-cli for developing and testing the desktop UI without a HiBy and before the
// real :cli module exists. It prints the JSON-lines output of docs/BYOS_CLI.md for the
// commands the scaffold uses: devices, status, df, ls, mkdir, mv, rm, connect, put.
//
// It never touches a device or the network. Its "SD card" is a JSON file:
//   BYOS_MOCK_STATE     path of the state file (default: <tmp>/byos-cli-mock-state.json)
//   BYOS_MOCK_SCENARIO  ok (default) | usb_storage | no_device | daemon_not_running | slow |
//                       silent_pair (no heartbeat while waiting for Allow)
//   BYOS_MOCK_DELAY_MS  extra delay before the result line (default 0)
//   BYOS_MOCK_PAIR_WAIT_MS   time until the fake user taps Allow (default 400)
//   BYOS_MOCK_HEARTBEAT_MS   progress repeat interval while waiting (default 5000)
// Options and paths can be mixed; "--" ends the options.

import { existsSync, readFileSync, renameSync, statSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';

const T0 = Date.now();
const SCENARIO = process.env.BYOS_MOCK_SCENARIO || 'ok';
const STATE = process.env.BYOS_MOCK_STATE || path.join(os.tmpdir(), 'byos-cli-mock-state.json');
const DELAY = Number(process.env.BYOS_MOCK_DELAY_MS || 0);
const PAIR_WAIT_MS = Number(process.env.BYOS_MOCK_PAIR_WAIT_MS || 400);
const HEARTBEAT_MS = Number(process.env.BYOS_MOCK_HEARTBEAT_MS || 5000);
const PAIR_CODE = '815365';
const VERSION = '0.0.0-mock';

const BTP = {
  E_PAIR_CLOSED: 6, E_PAIR_REJECTED: 7, E_BAD_PATH: 32, E_NOT_FOUND: 33, E_EXISTS: 34,
  E_NOT_DIR: 35, E_IS_DIR: 36, E_NOT_EMPTY: 37, E_NO_SPACE: 38, E_SD_UNAVAILABLE: 40,
};
const EXIT = {
  usage: 1, no_device: 2, auth: 3, remote: 4, local_io: 5, verify: 6, cancelled: 7,
  rejected: 8, timeout: 9, internal: 10,
};

function emit(event, fields = {}) {
  process.stdout.write(`${JSON.stringify({ v: 1, event, t: Date.now() - T0, ...fields })}\n`);
}

class Fail extends Error {
  constructor(code, message, btp, extra = {}) {
    super(message);
    this.code = code;
    this.btp = btp;
    this.extra = extra;
  }
}
const remoteErr = (name, message, p) =>
  new Fail('remote', message, { code: BTP[name], name }, p === undefined ? {} : { path: p });

// ---- stdin: prompt answers and cancellation ------------------------------------------

const answers = [];
let answerWaiter = null;
let cancelled = false;
const rl = readline.createInterface({ input: process.stdin });
rl.on('line', (line) => {
  let msg;
  try { msg = JSON.parse(line); } catch { msg = { answer: line }; }
  if (msg && msg.cancel === true) {
    cancelled = true;
    if (answerWaiter) answerWaiter(null);
    return;
  }
  const value = msg && typeof msg === 'object' && 'answer' in msg ? msg.answer : msg;
  if (answerWaiter) { const w = answerWaiter; answerWaiter = null; w(value); } else answers.push(value);
});
rl.on('close', () => { if (answerWaiter) { const w = answerWaiter; answerWaiter = null; w(null); } });

function nextAnswer() {
  if (answers.length) return Promise.resolve(answers.shift());
  return new Promise((resolve) => { answerWaiter = resolve; });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function tick(ms) {
  await sleep(ms);
  if (cancelled) throw new Fail('cancelled', 'Cancelled');
}

// ---- the fake card ---------------------------------------------------------------------

function file(size, mtime = 1767225600) { return { kind: 'file', size, mtime }; }
function dir(children, mtime = 1767225600) { return { kind: 'dir', mtime, children }; }

function defaultState() {
  return {
    total: 127865454592,
    root: dir({
      Music: dir({
        ABBA: dir({
          Arrival: dir({
            '01 When I Kissed the Teacher.flac': file(25165824),
            '02 Dancing Queen.flac': file(29360128),
            'cover.jpg': file(184320),
          }),
        }),
        'Daft Punk': dir({
          Discovery: dir({
            '01 One More Time.flac': file(41943040),
            '02 Aerodynamic.flac': file(26214400),
          }),
        }),
        'Ñandú': dir({ 'Canción (live).mp3': file(8388608) }),
      }),
      Audiobooks: dir({ 'Audio Book - Part 1.m4b': file(104857600) }),
      Mezclas: dir({ 'Mix 1.m3u': file(2048) }),
      ByOS: dir({ apps: dir({}), payload: dir({}) }),
      '.temp': dir({}),
    }),
  };
}

function load() {
  if (existsSync(STATE)) {
    try { return JSON.parse(readFileSync(STATE, 'utf8')); } catch { /* corrupt: start again */ }
  }
  return defaultState();
}

function save(state) {
  const tmp = `${STATE}.tmp`;
  writeFileSync(tmp, JSON.stringify(state));
  renameSync(tmp, STATE);
}

function used(node) {
  if (node.kind === 'file') return node.size;
  return Object.values(node.children).reduce((s, c) => s + used(c), 0);
}

function lookup(state, p) {
  let node = state.root;
  if (p === '') return node;
  for (const part of p.split('/')) {
    if (node.kind !== 'dir') throw remoteErr('E_NOT_DIR', `Not a folder: ${p}`, p);
    node = node.children[part];
    if (!node) throw remoteErr('E_NOT_FOUND', `Not found: ${p}`, p);
  }
  return node;
}

function parentOf(state, p) {
  const i = p.lastIndexOf('/');
  const parent = lookup(state, i < 0 ? '' : p.slice(0, i));
  if (parent.kind !== 'dir') throw remoteErr('E_NOT_DIR', `Not a folder: ${p}`, p);
  return { parent, name: i < 0 ? p : p.slice(i + 1) };
}

function checkRemote(p, allowEmpty = false) {
  if (p === '' && allowEmpty) return;
  const parts = p.split('/');
  const bad = p === '' || p.startsWith('/') || parts.some((c) => c === '' || c.startsWith('.') ||
    /[\u0000-\u001f\u007f\\:*?"<>|]/.test(c) || /[ .]$/.test(c));
  if (bad) throw remoteErr('E_BAD_PATH', `Invalid path: ${p}`, p);
}

// ---- device facts -----------------------------------------------------------------------

function sdState() {
  return SCENARIO === 'usb_storage' ? 'usb_storage' : 'mounted';
}

function usbDevice(extra = {}) {
  return {
    id: '0123456789abcdef', name: 'HiBy R3 II', transport: 'usb', address: '127.0.0.1:7340',
    serial: 'MOCK0001', paired: false, pairing_open: false, sd_state: sdState(),
    byos_version: '0.8.0', firmware: '0.8.0', ...extra,
  };
}

function connect() {
  if (SCENARIO === 'no_device') {
    throw new Fail('no_device', 'No HiBy found. Connect it with a USB cable or pair it over Wi-Fi.');
  }
  if (SCENARIO === 'daemon_not_running' && !opts.start) {
    emit('warn', { code: 'daemon_not_running', message: 'ByOS Sync is not running on the HiBy' });
    throw new Fail('no_device', 'Open Settings > ByOS Sync on the HiBy, or retry with --start.');
  }
  emit('device', usbDevice());
}

function requireSd() {
  if (sdState() !== 'mounted') {
    throw remoteErr('E_SD_UNAVAILABLE', "The HiBy's card is connected to the computer as a USB drive.");
  }
}

function dfFields(state) {
  const total = state.total;
  return {
    sd_state: sdState(), sd_fs: 'exfat', sd_total: total,
    sd_free: sdState() === 'mounted' ? total - used(state.root) : 0,
    data_total: 150994944, data_free: 25165824,
  };
}

// ---- commands ---------------------------------------------------------------------------

const commands = {
  async devices() {
    if (SCENARIO === 'no_device') return { count: 0 };
    await tick(80);
    emit('device', usbDevice({ reachable: true }));
    emit('device', {
      id: 'fedcba9876543210', name: 'HiBy R3 II (bedroom)', transport: 'wifi',
      address: '', paired: true, pairing_open: false, reachable: false,
      sd_state: 'unknown', byos_version: '0.8.0', firmware: '0.8.0',
    });
    return { count: 2 };
  },

  async status() {
    connect();
    const state = load();
    return {
      device: usbDevice(), battery: 76, charging: true, sd: dfFields(state),
      db: { staged: false, live_sha256: 'ab'.repeat(32), live_size: 2174976, last_apply: null },
      update: { state: 'none', version: null, permille: 0 },
      transport: 'usb', encrypted: false,
    };
  },

  async df() {
    connect();
    return dfFields(load());
  },

  async ls(args) {
    connect();
    requireSd();
    const p = args[0] === '/' ? '' : args[0] ?? ''; // "" or "/" = SD root
    checkRemote(p, true);
    const state = load();
    const node = lookup(state, p);
    if (node.kind !== 'dir') throw remoteErr('E_NOT_DIR', `Not a folder: ${p}`, p);
    let count = 0;
    const names = Object.keys(node.children).sort((a, b) => a.localeCompare(b));
    for (const name of names) {
      if (name.startsWith('.') && !opts.all) continue;
      const c = node.children[name];
      emit('entry', {
        name, path: p ? `${p}/${name}` : name, kind: c.kind,
        size: c.kind === 'file' ? c.size : 0, mtime: c.mtime,
      });
      count++;
    }
    return { count, path: p };
  },

  async mkdir(args) {
    connect();
    requireSd();
    const state = load();
    const created = [];
    for (const p of args) {
      checkRemote(p);
      const parts = p.split('/');
      let node = state.root;
      for (let i = 0; i < parts.length; i++) {
        const last = i === parts.length - 1;
        const child = node.children[parts[i]];
        if (!child) {
          if (!last && !opts.p) throw remoteErr('E_NOT_FOUND', `Not found: ${parts.slice(0, i + 1).join('/')}`, p);
          node.children[parts[i]] = dir({}, Math.floor(Date.now() / 1000));
          if (last) created.push(p);
        } else if (child.kind !== 'dir') {
          throw remoteErr('E_NOT_DIR', `Not a folder: ${parts.slice(0, i + 1).join('/')}`, p);
        } else if (last && !opts.p) {
          throw remoteErr('E_EXISTS', `Already exists: ${p}`, p);
        }
        node = node.children[parts[i]];
      }
    }
    save(state);
    return { created };
  },

  async mv(args) {
    connect();
    requireSd();
    const [src, dst] = args;
    if (!src || !dst) throw new Fail('usage', 'mv SRC DST');
    checkRemote(src);
    checkRemote(dst);
    const state = load();
    const from = parentOf(state, src);
    const node = from.parent.children[from.name];
    if (!node) throw remoteErr('E_NOT_FOUND', `Not found: ${src}`, src);
    const to = parentOf(state, dst);
    if (to.parent.children[to.name] && !opts.replace) throw remoteErr('E_EXISTS', `Already exists: ${dst}`, dst);
    delete from.parent.children[from.name];
    to.parent.children[to.name] = node;
    save(state);
    return { src, dst };
  },

  async rm(args) {
    connect();
    requireSd();
    const state = load();
    const deleted = [];
    for (const p of args) {
      checkRemote(p);
      const { parent, name } = parentOf(state, p);
      const node = parent.children[name];
      if (!node) throw remoteErr('E_NOT_FOUND', `Not found: ${p}`, p);
      if (node.kind === 'dir' && Object.keys(node.children).length) {
        if (!opts.r) throw remoteErr('E_NOT_EMPTY', `The folder is not empty: ${p}`, p);
        if (!opts.yes) {
          emit('prompt', { kind: 'confirm', message: `Delete folder ${p} and everything in it?`, path: p });
          const a = await nextAnswer();
          if (cancelled) throw new Fail('cancelled', 'Cancelled');
          if (a !== true && a !== 'yes' && a !== 'y') throw new Fail('cancelled', 'Not deleted');
        }
      }
      delete parent.children[name];
      deleted.push(p);
    }
    save(state);
    return { deleted };
  },

  async connect() {
    if (opts.forget) return { id: 'fedcba9876543210', name: 'HiBy R3 II (bedroom)', paired: false, transport: 'wifi', encrypted: false };
    if (!opts.pair) {
      connect();
      return { id: '0123456789abcdef', name: 'HiBy R3 II', paired: false, transport: 'usb', encrypted: false };
    }
    if (SCENARIO === 'no_device') throw new Fail('no_device', 'No HiBy found on this network.');
    await tick(150);
    emit('prompt', {
      kind: 'pair_code', message: 'Type the 6-digit code shown on the HiBy',
      device: 'HiBy R3 II', digits: 6,
    });
    const code = opts.code ?? await nextAnswer();
    if (cancelled) throw new Fail('cancelled', 'Cancelled');
    if (String(code ?? '').trim() !== PAIR_CODE) throw new Fail('auth', 'The code does not match.');
    // The user taps Allow after PAIR_WAIT_MS. The real CLI repeats the progress line every 5 s
    // (heartbeat); the silent_pair scenario leaves it out to test the caller's watchdog.
    const pairWait = { op: 'pair_wait', done: 0, total: 0, unit: 'none' };
    emit('progress', pairWait);
    for (let waited = 0; waited < PAIR_WAIT_MS;) {
      const step = Math.min(HEARTBEAT_MS, PAIR_WAIT_MS - waited);
      await tick(step);
      waited += step;
      if (waited < PAIR_WAIT_MS && SCENARIO !== 'silent_pair') emit('progress', pairWait);
    }
    return { id: 'fedcba9876543210', name: 'HiBy R3 II', paired: true, transport: 'wifi', encrypted: true };
  },

  async put(args) {
    if (args.length < 2) throw new Fail('usage', 'put LOCAL... REMOTE_DIR/');
    const target = args[args.length - 1];
    const locals = args.slice(0, -1);
    if (!target.endsWith('/')) throw new Fail('usage', 'The mock only supports a folder target ending in "/"');
    const dirPath = target.slice(0, -1);
    checkRemote(dirPath);
    connect();
    requireSd();
    const state = load();
    let folder = state.root;
    for (const part of dirPath.split('/')) {
      if (!folder.children[part]) {
        if (!opts.mkdir) throw remoteErr('E_NOT_FOUND', `Not found: ${dirPath}`, dirPath);
        folder.children[part] = dir({}, Math.floor(Date.now() / 1000));
      }
      folder = folder.children[part];
      if (folder.kind !== 'dir') throw remoteErr('E_NOT_DIR', `Not a folder: ${dirPath}`, dirPath);
    }
    const items = [];
    let bytes = 0;
    const step = SCENARIO === 'slow' ? 250 : 20;
    for (const local of locals) {
      let st;
      try { st = statSync(local); } catch {
        throw new Fail('local_io', `Cannot read ${local}`);
      }
      const name = path.basename(local).normalize('NFC');
      const remote = `${dirPath}/${name}`;
      checkRemote(remote);
      const existing = folder.children[name];
      if (existing && opts['skip-identical'] && existing.size === st.size) {
        items.push({ path: remote, result: 'skipped', sha256: '00'.repeat(32) });
        continue;
      }
      if (existing && !opts.overwrite && !opts['skip-identical']) {
        emit('warn', { code: 'exists', message: `Already on the HiBy: ${remote}` });
        items.push({ path: remote, result: 'failed', sha256: null, error: 'E_EXISTS' });
        continue;
      }
      emit('progress', { op: 'hash', path: remote, done: st.size, total: st.size, rate: 0, unit: 'bytes' });
      for (let i = 1; i <= 4; i++) {
        await tick(step);
        emit('progress', {
          op: 'put', path: remote, done: Math.round((st.size * i) / 4), total: st.size,
          rate: 4800000, unit: 'bytes',
        });
      }
      folder.children[name] = file(st.size, Math.floor(st.mtimeMs / 1000));
      bytes += st.size;
      items.push({ path: remote, result: 'written', sha256: '11'.repeat(32) });
    }
    save(state);
    const failed = items.filter((i) => i.result === 'failed').length;
    const res = {
      files: items.length, written: items.filter((i) => i.result === 'written').length,
      skipped: items.filter((i) => i.result === 'skipped').length, failed, bytes, items,
    };
    if (failed) throw new Fail('remote', `${failed} file(s) were not copied`, { code: BTP.E_EXISTS, name: 'E_EXISTS' }, res);
    return res;
  },
};

// ---- argument parsing ---------------------------------------------------------------------

const VALUE_OPTS = new Set(['--device', '--adb', '--local-port', '--config', '--name', '--timeout', '--wait', '--code', '--max-depth']);
const opts = {};
const positional = [];
{
  const argv = process.argv.slice(2);
  let onlyPaths = false;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    // "--" ends the options (BYOS_CLI section 2): everything after it is a path.
    if (onlyPaths) { positional.push(a); continue; }
    if (a === '--') { onlyPaths = true; continue; }
    if (VALUE_OPTS.has(a)) { opts[a.slice(2)] = argv[++i]; continue; }
    if (a === '-p' || a === '-r') { opts[a.slice(1)] = true; continue; }
    if (a.startsWith('--')) { opts[a.slice(2)] = true; continue; }
    positional.push(a);
  }
}

async function main() {
  const command = positional.shift();
  emit('start', { command: command ?? '', version: VERSION, protocol: 'BTP/1' });
  try {
    if (!command || !Object.hasOwn(commands, command)) throw new Fail('usage', `Unknown command: ${command ?? '(none)'}`);
    const result = await commands[command](positional);
    if (DELAY) await tick(DELAY);
    if (cancelled) throw new Fail('cancelled', 'Cancelled');
    emit('result', { ok: true, ...result });
    return 0;
  } catch (e) {
    if (!(e instanceof Fail)) {
      emit('error', { ok: false, code: 'internal', message: String(e && e.message || e) });
      return EXIT.internal;
    }
    emit('error', { ok: false, code: e.code, message: e.message, ...(e.btp ? { btp: e.btp } : {}), ...e.extra });
    return EXIT[e.code] ?? EXIT.internal;
  }
}

main().then((code) => {
  rl.close();
  process.exitCode = code;
  process.stdin.destroy();
});
