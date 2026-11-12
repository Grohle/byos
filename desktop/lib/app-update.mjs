// ByOS Desktop updates itself from the project's GitHub releases (docs/APP_UPDATES.md).
//
// The newest release of the repository is the ByOS firmware release, so the app lists the
// releases and takes the highest "desktop-vX.Y.Z" that carries byos-app-update.json. That index
// is signed with the project's Ed25519 release key (the format of docs/OTA.md: exactly one
// "signature": "ed25519:<128 hex>", signed over the file with those digits set to '0'). Nothing in
// a release is used before the signature is checked with the keys below; the index fixes the
// app, version, tag, file name, size and SHA-256, and the exe is kept only when it is exactly
// that file. Links are always built here from the fixed release address, never taken from the
// page or from GitHub's answer.
//
// Node.js only (fetch, crypto). The runner (server.mjs) drives one AppUpdater.

import { createHash, createPublicKey, verify as cryptoVerify } from 'node:crypto';
import { createReadStream, createWriteStream, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import path from 'node:path';

// The release keys built into the player (src/ota_net.inc.c OTA_PUBKEY_HEX / OTA_PUBKEY2_HEX)
// and into ByOS Install (core btp/UpdatePackage.RELEASE_KEYS). test/app-update.test.mjs checks
// that the three lists agree.
export const RELEASE_KEYS = Object.freeze(['6d87851ece77b81547cfbde491d9c46494dc99e70922c48115bfe5d9e854e06e']);

export const APP_ID = 'byos-desktop-windows';
export const TAG_PREFIX = 'desktop-v';
export const INDEX_NAME = 'byos-app-update.json';
export const PRODUCT = 'byos-app-update';
export const FORMAT = 1;
export const RELEASES_API = 'https://api.github.com/repos/Grohle/hibyos/releases?per_page=100';
export const DOWNLOADS = 'https://github.com/Grohle/hibyos/releases/download/';
export const MAX_INDEX = 64 * 1024;
export const MAX_LIST = 8 * 1024 * 1024;
export const MAX_ASSET = 512 * 1024 * 1024;
export const DAY_MS = 24 * 3600 * 1000;
// The release file is "ByOS.exe" (the version is in its properties and in the signed index). A
// downloaded update is kept as updates\ByOS-<version>.exe: the names the launcher accepts
// (desktop/windows/launcher.c, forward_to_newer).
export const EXE_RE = /^ByOS-(\d{1,6}(?:\.\d{1,6}){1,3})\.exe$/;
export const localName = (version) => `ByOS-${version}.exe`;

const VERSION_RE = /^\d{1,6}(\.\d{1,6}){1,3}$/;
const NAME_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$/;

export class UpdateError extends Error {
  // code: offline | http | rate_limited | untrusted | mismatch | storage | cancelled | unsupported
  constructor(code, message, extra = {}) {
    super(message);
    this.code = code;
    Object.assign(this, extra);
  }
}

export function isNewer(a, b) {
  const p = (v) => String(v ?? '').trim().replace(/^v/, '').split(/[.+-]/).map((x) => Number.parseInt(x, 10) || 0);
  const x = p(a); const y = p(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    if ((x[i] ?? 0) !== (y[i] ?? 0)) return (x[i] ?? 0) > (y[i] ?? 0);
  }
  return false;
}

export function tagVersion(tag) {
  if (typeof tag !== 'string' || !tag.startsWith(TAG_PREFIX)) return null;
  const v = tag.slice(TAG_PREFIX.length);
  return VERSION_RE.test(v) ? v : null;
}

// The newest published desktop release with an index, from GitHub's release list (an array).
export function newestRelease(list) {
  if (!Array.isArray(list)) throw new UpdateError('http', "GitHub's list of releases cannot be read");
  let best = null;
  for (const r of list) {
    if (!r || typeof r !== 'object' || r.draft === true || r.prerelease === true) continue;
    const version = tagVersion(r.tag_name);
    if (!version || !Array.isArray(r.assets) || !r.assets.some((a) => a && a.name === INDEX_NAME)) continue;
    if (!best || isNewer(version, best.version)) best = { tag: r.tag_name, version };
  }
  return best;
}

function publicKey(hex) {
  return createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: Buffer.from(hex, 'hex').toString('base64url') }, format: 'jwk' });
}

// docs/OTA.md: true if one of keys made the signature of this JSON text.
export function verifySignedJson(buf, keys) {
  const text = buf.toString('latin1');
  if ((text.match(/"signature"/g) ?? []).length !== 1) return false;
  const m = /"signature"\s*:\s*"ed25519:([0-9A-Fa-f]{128})"/.exec(text);
  if (!m) return false;
  const start = m.index + m[0].indexOf(m[1]);
  const sig = Buffer.from(m[1], 'hex');
  const msg = Buffer.from(buf);
  msg.fill('0'.charCodeAt(0), start, start + 128);
  for (const k of keys) {
    if (!/^[0-9A-Fa-f]{64}$/.test(k) || /^0+$/.test(k)) continue;
    try {
      if (cryptoVerify(null, msg, publicKey(k.toLowerCase()), sig)) return true;
    } catch { /* a key that is not a point */ }
  }
  return false;
}

// Checks the signature first, then reads the index of the release `tag`.
export function parseIndex(buf, keys, tag, downloads = DOWNLOADS) {
  const bad = (why) => new UpdateError('untrusted', why);
  if (buf.length > MAX_INDEX) throw bad('the update description is too large');
  if (!verifySignedJson(buf, keys)) throw bad('the update description is not signed by ByOS');
  let m;
  try { m = JSON.parse(buf.toString('utf8')); } catch { throw bad('the update description is not valid JSON'); }
  if (!m || typeof m !== 'object' || Array.isArray(m)) throw bad('the update description is not valid');
  if (m.product !== PRODUCT) throw bad('this is not an app update description');
  if (m.format !== FORMAT) throw bad('this update needs a newer version of the app to be read');
  if (m.app !== APP_ID) throw bad('the update description is for another app');
  if (typeof m.version !== 'string' || !VERSION_RE.test(m.version)) throw bad('the update description has no valid version');
  if (m.tag !== tag || tagVersion(m.tag) !== m.version) throw bad('the update description belongs to another release');
  if (typeof m.asset !== 'string' || !NAME_RE.test(m.asset) || !m.asset.endsWith('.exe') || m.asset.includes('..')) {
    throw bad('unexpected file name in the update description');
  }
  if (!Number.isSafeInteger(m.size) || m.size <= 0 || m.size > MAX_ASSET) throw bad('unexpected size in the update description');
  if (typeof m.sha256 !== 'string' || !/^[0-9a-fA-F]{64}$/.test(m.sha256)) throw bad('unexpected SHA-256 in the update description');
  if (m.min_os !== undefined && (typeof m.min_os !== 'string' || !/^\d{1,6}(\.\d{1,6}){0,3}$/.test(m.min_os))) throw bad('unexpected minimum system');
  const notes = {};
  if (m.notes && typeof m.notes === 'object') {
    for (const [k, v] of Object.entries(m.notes)) if (/^[a-z]{2}$/.test(k) && typeof v === 'string') notes[k] = v.slice(0, 4000);
  }
  return {
    version: m.version, tag: m.tag, asset: m.asset, size: m.size, sha256: m.sha256.toLowerCase(),
    minOs: m.min_os ?? null, published: typeof m.published === 'string' ? m.published.slice(0, 32) : null, notes,
    url: `${downloads}${m.tag}/${m.asset}`,
  };
}

export async function fileSha256(file) {
  const h = createHash('sha256');
  for await (const c of createReadStream(file)) h.update(c);
  return h.digest('hex');
}

// Windows' own version, "10.0.19045" (null elsewhere).
export function osVersionOk(minOs, release) {
  if (!minOs) return true;
  return !isNewer(minOs, release);
}

export class AppUpdater {
  // options: version (this app; null = not the Windows program), home (%LOCALAPPDATA%\ByOS Desktop),
  // keys, api, downloads, fetch, osRelease, now
  constructor(o = {}) {
    this.version = o.version ?? null;
    this.home = o.home ?? null;
    this.dir = this.home ? path.join(this.home, 'updates') : null;
    this.keys = o.keys ?? RELEASE_KEYS;
    this.api = o.api ?? RELEASES_API;
    this.downloads = o.downloads ?? DOWNLOADS;
    this.fetch = o.fetch ?? globalThis.fetch;
    this.osRelease = o.osRelease ?? null;
    this.now = o.now ?? (() => Date.now());
    this.state = 'idle'; // idle | checking | uptodate | available | downloading | ready | failed
    this.release = null;
    this.file = null;
    this.done = 0;
    this.total = 0;
    this.error = null;
    this.abort = null;
    this.busy = null;
  }

  get supported() {
    return !!(this.version && this.dir);
  }

  settings() {
    const d = { auto: true, lastCheck: 0 };
    if (!this.dir) return d;
    try {
      const s = JSON.parse(readFileSync(path.join(this.dir, 'settings.json'), 'utf8'));
      return { auto: s.auto !== false, lastCheck: Number(s.lastCheck) || 0 };
    } catch { return d; }
  }

  saveSettings(patch) {
    if (!this.dir) return this.settings();
    const s = { ...this.settings(), ...patch };
    try {
      mkdirSync(this.dir, { recursive: true });
      writeFileSync(path.join(this.dir, 'settings.json'), JSON.stringify(s));
    } catch { /* read-only profile: the next start checks again */ }
    return s;
  }

  // What the page shows. Nothing in it is a path.
  status() {
    const s = this.settings();
    const r = this.release;
    return {
      supported: this.supported, current: this.version, state: this.state, auto: s.auto, lastCheck: s.lastCheck || null,
      latest: r?.version ?? null, size: r?.size ?? null, notes: r?.notes ?? null, published: r?.published ?? null,
      done: this.done, total: this.total, error: this.error,
    };
  }

  // The daily check is due (auto on and the last one more than a day ago).
  due() {
    const s = this.settings();
    return this.supported && s.auto && !(this.now() - s.lastCheck >= 0 && this.now() - s.lastCheck < DAY_MS);
  }

  async #get(url, max, signal, api = false) {
    let res;
    try {
      res = await this.fetch(url, {
        signal, redirect: 'follow',
        headers: api
          ? { 'User-Agent': 'ByOS-Desktop', Accept: 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28' }
          : { 'User-Agent': 'ByOS-Desktop', 'Accept-Encoding': 'identity' },
      });
    } catch (e) {
      if (signal?.aborted) throw new UpdateError('cancelled', 'cancelled');
      throw new UpdateError('offline', `no connection: ${e.cause?.code ?? e.message}`);
    }
    if (api && (res.status === 429 || (res.status === 403 && res.headers.get('x-ratelimit-remaining') === '0'))) {
      const retry = Number(res.headers.get('retry-after'));
      const reset = Number(res.headers.get('x-ratelimit-reset'));
      const wait = Number.isFinite(retry) && retry > 0 ? retry : Number.isFinite(reset) && reset > 0 ? reset - Math.floor(this.now() / 1000) : 3600;
      const minutes = Math.max(1, Math.ceil(wait / 60));
      throw new UpdateError('rate_limited', `GitHub limits how often it can be asked; try again in ${minutes} min`, { minutes });
    }
    if (res.status !== 200) throw new UpdateError('http', `${api ? 'GitHub' : 'the server'} answered ${res.status}`, { status: res.status });
    const len = Number(res.headers.get('content-length'));
    if (Number.isFinite(len) && len > max) throw new UpdateError('http', 'the answer is too long');
    const chunks = [];
    let n = 0;
    for await (const c of res.body) {
      n += c.length;
      if (n > max) throw new UpdateError('http', 'the answer is too long');
      chunks.push(Buffer.from(c));
    }
    return Buffer.concat(chunks);
  }

  // Looks for a newer version. Resolves the status; failures become state "failed" (quiet: no change).
  async check({ quiet = false } = {}) {
    if (!this.supported) return this.status();
    if (this.state === 'checking' || this.state === 'downloading') return this.status();
    if (this.state === 'ready') return this.status();
    const before = { state: this.state, error: this.error };
    this.state = 'checking';
    this.error = null;
    this.abort = new AbortController();
    try {
      const list = await this.#get(this.api, MAX_LIST, this.abort.signal, true);
      let parsed;
      try { parsed = JSON.parse(list.toString('utf8')); } catch { throw new UpdateError('http', "GitHub's list of releases cannot be read"); }
      const newest = newestRelease(parsed);
      let r = null;
      if (newest && isNewer(newest.version, this.version)) {
        const idx = await this.#get(`${this.downloads}${newest.tag}/${INDEX_NAME}`, MAX_INDEX, this.abort.signal);
        r = parseIndex(idx, this.keys, newest.tag, this.downloads);
        if (!isNewer(r.version, this.version)) r = null;
      }
      this.saveSettings({ lastCheck: this.now() });
      if (r && this.osRelease && !osVersionOk(r.minOs, this.osRelease)) {
        throw new UpdateError('unsupported', `ByOS Desktop ${r.version} needs Windows ${r.minOs} or newer`);
      }
      this.release = r;
      this.state = r ? 'available' : 'uptodate';
      if (r) this.#readyIfThere(r);
      else this.cleanup();
    } catch (e) {
      if (quiet) Object.assign(this, before);
      else {
        this.state = 'failed';
        this.error = { code: e.code ?? 'internal', message: e.message, minutes: e.minutes };
      }
    } finally {
      this.abort = null;
    }
    return this.status();
  }

  // A file left by an earlier run that is already the signed one.
  #readyIfThere(r) {
    const f = path.join(this.dir, localName(r.version));
    try {
      if (statSync(f).size !== r.size) return;
    } catch { return; }
    this.busy = fileSha256(f).then((sha) => {
      if (sha === r.sha256 && this.release === r && this.state === 'available') {
        this.file = f;
        this.state = 'ready';
        this.done = this.total = r.size;
      }
    }).catch(() => {}).finally(() => { this.busy = null; });
  }

  // Downloads the newer exe into updates\ and checks its size and SHA-256 against the signed index.
  async download() {
    const r = this.release;
    if (!this.supported || !r || this.state !== 'available') return this.status();
    this.state = 'downloading';
    this.error = null;
    this.done = 0;
    this.total = r.size;
    this.abort = new AbortController();
    const dest = path.join(this.dir, localName(r.version));
    const part = `${dest}.part`;
    try {
      mkdirSync(this.dir, { recursive: true });
      let res;
      try {
        res = await this.fetch(r.url, { signal: this.abort.signal, redirect: 'follow', headers: { 'User-Agent': 'ByOS-Desktop', 'Accept-Encoding': 'identity' } });
      } catch (e) {
        if (this.abort.signal.aborted) throw new UpdateError('cancelled', 'cancelled');
        throw new UpdateError('offline', `no connection: ${e.cause?.code ?? e.message}`);
      }
      if (res.status !== 200) throw new UpdateError('http', `the server answered ${res.status}`, { status: res.status });
      if ((res.headers.get('content-type') ?? '').toLowerCase().startsWith('text/html')) throw new UpdateError('mismatch', 'the link shows a web page instead of the file');
      const len = Number(res.headers.get('content-length'));
      if (Number.isFinite(len) && res.headers.has('content-length') && len !== r.size) {
        throw new UpdateError('mismatch', `the link offers a file of ${len} bytes, not the signed ${r.size}`);
      }
      const h = createHash('sha256');
      const out = createWriteStream(part);
      const closed = new Promise((resolve, reject) => { out.on('close', resolve); out.on('error', reject); });
      try {
        for await (const c of res.body) {
          this.done += c.length;
          if (this.done > r.size) throw new UpdateError('mismatch', 'the server sends more than the signed file');
          h.update(c);
          if (!out.write(c)) await new Promise((resolve) => out.once('drain', resolve));
        }
      } catch (e) {
        if (this.abort.signal.aborted) throw new UpdateError('cancelled', 'cancelled');
        if (e instanceof UpdateError) throw e;
        throw new UpdateError('offline', `the connection broke after ${this.done} bytes`);
      } finally {
        out.end();
        await closed.catch(() => {});
      }
      if (this.done !== r.size || h.digest('hex') !== r.sha256) throw new UpdateError('mismatch', `the downloaded file is not the signed ${r.asset}`);
      rmSync(dest, { force: true });
      renameSync(part, dest);
      this.file = dest;
      this.state = 'ready';
    } catch (e) {
      rmSync(part, { force: true });
      if (e.code === 'cancelled') this.state = 'available';
      else {
        this.state = 'failed';
        this.error = { code: e instanceof UpdateError ? e.code : 'storage', message: e.message };
      }
    } finally {
      this.abort = null;
    }
    return this.status();
  }

  cancel() {
    this.abort?.abort();
  }

  // On confirm: the exe is checked once more, then named for the launcher (updates\latest.txt) and
  // the restart is asked for (updates\restart). The launcher starts it once this runner has ended.
  async apply() {
    if (this.busy) await this.busy;
    const r = this.release;
    if (!this.supported || this.state !== 'ready' || !r || !this.file) throw new UpdateError('mismatch', 'no checked update to install');
    let size = -1;
    try { size = statSync(this.file).size; } catch { /* gone */ }
    if (size !== r.size || (await fileSha256(this.file)) !== r.sha256) {
      this.state = 'failed';
      this.error = { code: 'mismatch', message: 'the downloaded update changed; download it again' };
      throw new UpdateError('mismatch', this.error.message);
    }
    writeFileSync(path.join(this.dir, 'latest.txt'), `${localName(r.version)}\n`);
    writeFileSync(path.join(this.dir, 'restart'), `${r.version}\n`);
    return { version: r.version };
  }

  // Older exes and leftovers in updates\ (never the newest one the launcher forwards to).
  cleanup() {
    if (!this.dir || !this.version) return 0;
    let n = 0;
    let names = [];
    try { names = readdirSync(this.dir); } catch { return 0; }
    for (const name of names) {
      const m = EXE_RE.exec(name.replace(/\.part$/, ''));
      if (!m) continue;
      if (name.endsWith('.part') || !isNewer(m[1], this.version)) {
        try { rmSync(path.join(this.dir, name), { force: true }); n++; } catch { /* in use */ }
      }
    }
    // latest.txt naming a version that is not newer any more is no longer needed
    try {
      const latest = readFileSync(path.join(this.dir, 'latest.txt'), 'utf8').trim();
      const m = EXE_RE.exec(latest);
      if (!m || !isNewer(m[1], this.version)) rmSync(path.join(this.dir, 'latest.txt'), { force: true });
    } catch { /* none */ }
    return n;
  }
}
