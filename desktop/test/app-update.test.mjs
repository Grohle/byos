// The app's own update (lib/app-update.mjs, server.mjs /api/app-update): GitHub's release list,
// the signed index, the verified download and the hand-over to the launcher. No network: a fake
// GitHub serves everything from memory, signed with a key made for the test.
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync, mkdirSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  AppUpdater, isNewer, newestRelease, parseIndex, RELEASE_KEYS, tagVersion, verifySignedJson, localName, EXE_RE,
} from '../lib/app-update.mjs';
import { DEMO_API, DEMO_DOWNLOADS, demoRelease, indexText, newKey, signJson } from '../mock/app-update-mock.mjs';
import { buildArgs } from '../lib/cli.mjs';
import { createApp } from '../server.mjs';
import { STRINGS } from '../ui/i18n.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const tmp = mkdtempSync(path.join(os.tmpdir(), 'byos-appupd-test-'));
test.after(() => rmSync(tmp, { recursive: true, force: true }));
let n = 0;
const home = () => path.join(tmp, `home-${n++}`);

const key = newKey();
const sha = 'ab'.repeat(32);
const signed = (o) => signJson(indexText({ version: '0.9.2', size: 4000, sha256: sha, ...o }), key.privateKey);

test('versions and tags', () => {
  assert.ok(isNewer('0.10.0', '0.9.9'));
  assert.ok(!isNewer('0.9.0', '0.9.0'));
  assert.ok(!isNewer('0.8.9', '0.9.0'));
  assert.equal(tagVersion('desktop-v1.2.3'), '1.2.3');
  assert.equal(tagVersion('install-v1.2.3'), null);
  assert.equal(tagVersion('desktop-v1'), null);
  assert.equal(tagVersion('desktop-v1.2.3-rc1'), null);
  assert.equal(localName('0.9.1'), 'ByOS-0.9.1.exe');
  assert.ok(EXE_RE.test('ByOS-0.9.1.exe'));
  assert.ok(!EXE_RE.test('ByOS.exe'));
});

test('the newest desktop release is found among the firmware and Android releases', () => {
  const rel = (tag, assets = ['byos-app-update.json'], extra = {}) => ({ tag_name: tag, draft: false, prerelease: false, assets: assets.map((name) => ({ name })), ...extra });
  const list = [
    rel('v0.9.1', ['hibyos-update.json']), rel('install-v1.2.0'), rel('desktop-v0.9.0'), rel('desktop-v0.10.0'),
    rel('desktop-v0.11.0', ['ByOS.exe']), rel('desktop-v0.12.0', undefined, { draft: true }), rel('desktop-v0.13.0', undefined, { prerelease: true }),
    null, 'x',
  ];
  assert.deepEqual(newestRelease(list), { tag: 'desktop-v0.10.0', version: '0.10.0' });
  assert.equal(newestRelease([rel('v0.9.1')]), null);
  assert.throws(() => newestRelease({ message: 'Not Found' }));
});

test('only an index signed with the release key, for this app and release, is read', () => {
  const r = parseIndex(signed(), [key.pub], 'desktop-v0.9.2', 'https://x.invalid/dl/');
  assert.equal(r.version, '0.9.2');
  assert.equal(r.asset, 'ByOS.exe');
  assert.equal(r.size, 4000);
  assert.equal(r.url, 'https://x.invalid/dl/desktop-v0.9.2/ByOS.exe');
  assert.equal(r.notes.fr, 'Synchro plus rapide et un bouton Rechercher des mises à jour.');
  const bad = (buf, keys = [key.pub], tag = 'desktop-v0.9.2') => assert.throws(() => parseIndex(buf, keys, tag), (e) => e.code === 'untrusted');
  bad(signJson(indexText({ version: '0.9.2', size: 4000, sha256: sha }), newKey().privateKey)); // another key
  bad(Buffer.from(signed().toString().replace('4000', '4001'))); // changed after signing
  bad(signed(), []);
  bad(signed(), ['0'.repeat(64)]);
  bad(signed(), RELEASE_KEYS); // the real key did not sign the test index
  bad(Buffer.from('<html>Not Found</html>'));
  bad(signed({ app: 'byos-install-android' }));
  bad(signed(), [key.pub], 'desktop-v0.9.3'); // replayed under another tag
  bad(signed({ tag: 'desktop-v0.9.1' }), [key.pub], 'desktop-v0.9.1');
  bad(signed({ product: 'hibyos-install-bundle' }));
  bad(signed({ format: 2 }));
  bad(signed({ asset: '../evil.exe' }));
  bad(signed({ asset: 'ByOS.apk' }));
  bad(signed({ asset: 'a\\b.exe' }));
  bad(signed({ size: 0 }));
  bad(signed({ sha256: 'xyz' }));
  bad(signed({ minOs: 'ten' }));
  assert.ok(verifySignedJson(signed(), ['11'.repeat(32), key.pub]));
});

test('the built-in release keys are the player\'s and the Android app\'s', () => {
  const c = readFileSync(path.join(REPO, 'src', 'ota_net.inc.c'), 'utf8');
  const player = [...c.matchAll(/#define OTA_PUBKEY2?_HEX "([0-9A-Fa-f]{64})"/g)].map((m) => m[1].toLowerCase());
  const kt = readFileSync(path.join(REPO, 'android', 'hibyos-install', 'core', 'src', 'main', 'kotlin', 'org', 'hibyos', 'install', 'core', 'btp', 'UpdatePackage.kt'), 'utf8');
  const android = [...(/RELEASE_KEYS = listOf\(([^)]*)\)/.exec(kt)[1].matchAll(/"([0-9a-f]{64})"/g))].map((m) => m[1]);
  assert.deepEqual([...RELEASE_KEYS], player);
  assert.deepEqual([...RELEASE_KEYS], android);
});

function updater(d, h = home(), extra = {}) {
  return new AppUpdater({ version: d.current, home: h, keys: [d.key.pub], api: DEMO_API, downloads: DEMO_DOWNLOADS, fetch: d.fetch, ...extra });
}

test('check, verified download, and the hand-over to the launcher', async () => {
  const d = demoRelease({ current: '0.9.0', latest: '0.9.1', size: 300000, delayMs: 0 });
  const h = home();
  const u = updater(d, h);
  assert.ok(u.due());
  let s = await u.check();
  assert.equal(s.state, 'available');
  assert.equal(s.latest, '0.9.1');
  assert.equal(s.size, 300000);
  assert.ok(!u.due(), 'checked today');
  assert.ok(!JSON.stringify(s).includes(h), 'no path goes to the page');
  s = await u.download();
  assert.equal(s.state, 'ready');
  const exe = path.join(h, 'updates', 'ByOS-0.9.1.exe');
  assert.deepEqual(readFileSync(exe), d.exe);
  assert.ok(!existsSync(`${exe}.part`));
  const r = await u.apply();
  assert.equal(r.version, '0.9.1');
  assert.equal(readFileSync(path.join(h, 'updates', 'latest.txt'), 'utf8'), 'ByOS-0.9.1.exe\n');
  assert.ok(existsSync(path.join(h, 'updates', 'restart')));

  // the next start of 0.9.1: older files go, the new exe stays until a newer one
  const after = new AppUpdater({ version: '0.9.1', home: h, fetch: d.fetch });
  writeFileSync(path.join(h, 'updates', 'ByOS-0.8.0.exe'), 'old');
  after.cleanup();
  assert.deepEqual(readdirSync(path.join(h, 'updates')).filter((x) => x.endsWith('.exe') || x === 'latest.txt').sort(), []);

  // a file already downloaded and right is not downloaded again
  const d2 = demoRelease({ current: '0.9.0', latest: '0.9.1', size: 1000, delayMs: 0 });
  const h2 = home();
  mkdirSync(path.join(h2, 'updates'), { recursive: true });
  writeFileSync(path.join(h2, 'updates', 'ByOS-0.9.1.exe'), d2.exe);
  const u2 = updater(d2, h2);
  await u2.check();
  await u2.busy;
  assert.equal(u2.status().state, 'ready');
  assert.ok(!d2.fetch.calls.some((c) => c.endsWith('/ByOS.exe')));
});

test('up to date: the index is not even downloaded', async () => {
  const d = demoRelease({ current: '0.9.1', latest: '0.9.1', size: 1000, delayMs: 0 });
  const u = updater(d);
  assert.equal((await u.check()).state, 'uptodate');
  assert.deepEqual(d.fetch.calls, [DEMO_API]);
});

test('a file that is not the signed one is never kept', async () => {
  const d = demoRelease({ size: 200000, delayMs: 0 });
  const url = `${DEMO_DOWNLOADS}desktop-v0.9.1/ByOS.exe`;
  const h = home();
  const u = updater(d, h);
  await u.check();
  d.fetch.files.set(url, Buffer.from(d.exe).fill(1, 100, 101));
  let s = await u.download();
  assert.equal(s.state, 'failed');
  assert.equal(s.error.code, 'mismatch');
  assert.deepEqual(readdirSync(path.join(h, 'updates')).filter((x) => x.includes('.exe')), []);
  await assert.rejects(() => u.apply(), (e) => e.code === 'mismatch');
  assert.ok(!existsSync(path.join(h, 'updates', 'latest.txt')));
  // another size
  u.state = 'available';
  d.fetch.files.set(url, d.exe.subarray(1));
  s = await u.download();
  assert.equal(s.error.code, 'mismatch');
  // a changed index is refused before anything is downloaded
  const u3 = updater(d, home());
  const idx = `${DEMO_DOWNLOADS}desktop-v0.9.1/byos-app-update.json`;
  d.fetch.files.set(idx, Buffer.from(d.fetch.files.get(idx).toString().replace('"size": 200000', '"size": 200001')));
  d.fetch.calls.length = 0;
  s = await u3.check();
  assert.equal(s.state, 'failed');
  assert.equal(s.error.code, 'untrusted');
  assert.ok(!d.fetch.calls.some((c) => c.endsWith('.exe')));
  // the exe changed on disk between download and restart
  const d4 = demoRelease({ size: 1000, delayMs: 0 });
  const h4 = home();
  const u4 = updater(d4, h4);
  await u4.check();
  await u4.download();
  writeFileSync(path.join(h4, 'updates', 'ByOS-0.9.1.exe'), 'changed');
  await assert.rejects(() => u4.apply(), (e) => e.code === 'mismatch');
  assert.ok(!existsSync(path.join(h4, 'updates', 'restart')));
});

test("GitHub's rate limit, no connection, and the quiet daily check", async () => {
  const d = demoRelease({ size: 1000, delayMs: 0 });
  d.fetch.apiStatus = 403;
  d.fetch.apiHeaders = { 'x-ratelimit-remaining': '0', 'x-ratelimit-reset': String(Math.floor(Date.now() / 1000) + 600) };
  const u = updater(d);
  let s = await u.check();
  assert.equal(s.state, 'failed');
  assert.equal(s.error.code, 'rate_limited');
  assert.equal(s.error.minutes, 10);
  // the quiet check leaves no error behind
  const q = updater(d);
  s = await q.check({ quiet: true });
  assert.equal(s.state, 'idle');
  assert.equal(s.error, null);
  const off = new AppUpdater({ version: '0.9.0', home: home(), fetch: async () => { throw new TypeError('fetch failed'); } });
  s = await off.check();
  assert.equal(s.error.code, 'offline');
  // daily: due again after a day, never when turned off
  let now = 1_000_000_000_000;
  const dd = demoRelease({ current: '0.9.1', latest: '0.9.1', size: 10, delayMs: 0 });
  const t = updater(dd, home(), { now: () => now });
  assert.ok(t.due());
  await t.check();
  assert.ok(!t.due());
  now += 25 * 3600 * 1000;
  assert.ok(t.due());
  t.saveSettings({ auto: false });
  assert.ok(!t.due());
  // too new for this Windows
  const w = updater(demoRelease({ size: 10, delayMs: 0 }), home(), { osRelease: '10.0.10240' });
  s = await w.check();
  assert.equal(s.error.code, 'unsupported');
  // outside the Windows program (no version, no home): nothing is checked
  assert.equal((await new AppUpdater({}).check()).supported, false);
});

test('the runner: status, check, download, install (demo and launcher mode)', async () => {
  const tdir = mkdtempSync(path.join(tmp, 'srv-'));
  const env = { ...process.env, BYOS_MOCK_STATE: path.join(tdir, 'state.json') };
  const H = { 'X-ByOS-Token': 'tok', 'Content-Type': 'application/json' };
  const d = demoRelease({ size: 50000, delayMs: 0 });
  const h = home();
  let restarted = 0;
  const app = createApp({
    mock: true, env, token: 'tok', appHome: h, onRestart: () => { restarted++; },
    updater: updater(d, h),
  });
  const base = await app.listen(0);
  try {
    const get = async (p) => (await fetch(new URL(p, base), { headers: H })).json();
    const post = async (p, body = {}) => {
      const r = await fetch(new URL(p, base), { method: 'POST', headers: H, body: JSON.stringify(body) });
      return { status: r.status, json: await r.json() };
    };
    assert.equal((await fetch(new URL('/api/app-update', base))).status, 403);
    assert.equal((await get('/api/app-update')).state, 'idle');
    assert.equal((await post('/api/app-update/check', { quiet: true })).json.state, 'available');
    // quiet again the same day: nothing is asked
    const calls = d.fetch.calls.length;
    await post('/api/app-update/check', { quiet: true });
    assert.equal(d.fetch.calls.length, calls);
    assert.equal((await post('/api/app-update/auto', { on: 'yes' })).status, 400);
    assert.equal((await post('/api/app-update/auto', { on: false })).json.auto, false);
    // install before download: refused
    assert.equal((await post('/api/app-update/install')).status, 409);
    // the page cannot name a file or a link: extra fields are ignored
    await post('/api/app-update/download', { url: 'https://evil.invalid/x.exe', file: 'C:\\x.exe' });
    for (let i = 0; i < 100 && (await get('/api/app-update')).state === 'downloading'; i++) await new Promise((r) => setTimeout(r, 20));
    assert.equal((await get('/api/app-update')).state, 'ready');
    assert.ok(!d.fetch.calls.some((c) => c.includes('evil')));
    // demo mode: the files are written, nothing restarts
    const ins = await post('/api/app-update/install');
    assert.equal(ins.status, 200);
    assert.equal(ins.json.restarting, false);
    assert.ok(existsSync(path.join(h, 'updates', 'restart')));
  } finally {
    await app.close();
  }
  // the Windows program (not the demo): the runner ends and the launcher takes over
  const d2 = demoRelease({ size: 1000, delayMs: 0 });
  const h2 = home();
  const real = createApp({ env, token: 'tok', cli: path.join(REPO, 'desktop', 'mock', 'byos-cli-mock.mjs'), appHome: h2, appVersion: '0.9.0', onRestart: () => { restarted++; }, updater: updater(d2, h2) });
  const b2 = await real.listen(0);
  try {
    const post = async (p) => (await fetch(new URL(p, b2), { method: 'POST', headers: H, body: '{}' })).json();
    await post('/api/app-update/check');
    await post('/api/app-update/download');
    for (let i = 0; i < 100 && real.updater?.state === 'downloading'; i++) await new Promise((r) => setTimeout(r, 20));
    await new Promise((r) => setTimeout(r, 100));
    const r = await post('/api/app-update/install');
    assert.equal(r.restarting, true);
    await new Promise((res) => setTimeout(res, 500));
    assert.equal(restarted, 1);
    assert.equal(readFileSync(path.join(h2, 'updates', 'latest.txt'), 'utf8'), 'ByOS-0.9.1.exe\n');
  } finally {
    await real.close();
  }
});

test('the demo updater finds a signed 0.9.1 without any network', async () => {
  const tdir = mkdtempSync(path.join(tmp, 'demo-'));
  const app = createApp({ mock: true, env: { ...process.env, BYOS_MOCK_STATE: path.join(tdir, 's.json') }, token: 't' });
  const base = await app.listen(0);
  try {
    const r = await fetch(new URL('/api/app-update/check', base), { method: 'POST', headers: { 'X-ByOS-Token': 't', 'Content-Type': 'application/json' }, body: '{}' });
    const s = await r.json();
    assert.equal(s.current, '0.9.0');
    assert.equal(s.latest, '0.9.1');
    assert.equal(s.state, 'available');
  } finally {
    await app.close();
  }
});

test('Start Over and the ByOS check of the Updates tab', () => {
  assert.deepEqual(buildArgs('build-clean', { dir: 'C:\\x' }), ['build-clean']);
  assert.deepEqual(buildArgs('bundle-update', {}), ['bundle-update']);
});

test('the update texts exist in English, French and Spanish', () => {
  const keys = Object.keys(STRINGS.en).filter((k) => /^(au|bc|so)[A-Z]/.test(k));
  assert.ok(keys.length > 25, String(keys.length));
  const ph = (s) => [...String(s).matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');
  for (const l of ['fr', 'es']) {
    assert.deepEqual(keys.filter((k) => !(k in STRINGS[l])), [], l);
    for (const k of keys) assert.equal(ph(STRINGS[l][k]), ph(STRINGS.en[k]), `${l}.${k}`);
  }
});
