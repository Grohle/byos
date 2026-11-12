import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { existsSync, mkdirSync, mkdtempSync, readdirSync, rmSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createApp, sweepStaleTemp } from '../server.mjs';

const tmp = mkdtempSync(path.join(os.tmpdir(), 'byos-server-test-'));
const env = { ...process.env, BYOS_MOCK_STATE: path.join(tmp, 'state.json') };
const app = createApp({ mock: true, env, token: 'testtoken' });
let base;
test.before(async () => { base = await app.listen(0); });
test.after(async () => { await app.close(); rmSync(tmp, { recursive: true, force: true }); });

const H = { 'X-ByOS-Token': 'testtoken', 'Content-Type': 'application/json' };

async function post(p, body, headers = H) {
  const r = await fetch(new URL(p, base), { method: 'POST', headers, body: JSON.stringify(body) });
  return { status: r.status, json: await r.json() };
}

// Reads a job's server-sent events until "end".
async function events(id, at = base, token = 'testtoken') {
  const r = await fetch(new URL(`/api/jobs/${id}/events?token=${token}`, at));
  assert.equal(r.headers.get('content-type'), 'text/event-stream; charset=utf-8');
  const text = await r.text();
  const out = [];
  let final = null;
  for (const block of text.split('\n\n')) {
    if (!block.trim()) continue;
    const name = /^event: (.*)$/m.exec(block)?.[1] ?? 'message';
    const data = JSON.parse(/^data: (.*)$/m.exec(block)[1]);
    if (name === 'end') final = data;
    else if (name === 'message') out.push(data);
  }
  return { events: out, final };
}

test('the page is served only to whoever has the token, with a strict CSP', async () => {
  // No token, a wrong token: refused (the page holds the token, so it is not public).
  assert.equal((await fetch(base)).status, 403);
  assert.equal((await fetch(new URL('/?t=wrong', base))).status, 403);
  // The launch address sets an HttpOnly cookie and redirects to "/".
  const first = await fetch(new URL('/?t=testtoken', base), { redirect: 'manual' });
  assert.equal(first.status, 303);
  assert.equal(first.headers.get('location'), '/');
  const cookie = first.headers.get('set-cookie');
  assert.match(cookie, /HttpOnly/);
  assert.match(cookie, /SameSite=Strict/);
  const pair = cookie.split(';')[0];
  const r = await fetch(base, { headers: { Cookie: pair } });
  const html = await r.text();
  assert.equal(r.status, 200);
  // The cookie also opens the API.
  assert.equal((await fetch(new URL('/api/info', base), { headers: { Cookie: pair } })).status, 200);
  assert.equal((await fetch(new URL('/api/info', base), { headers: { Cookie: `${pair}x` } })).status, 403);
  assert.match(html, /content="testtoken"/);
  assert.match(r.headers.get('content-security-policy'), /default-src 'self'/);
  const js = await fetch(new URL('/btp-path.js', base));
  assert.equal(js.status, 200);
  assert.match(await js.text(), /export function checkPath/);
});

test('the API needs the token, the right Host and the right Origin', async () => {
  assert.equal((await fetch(new URL('/api/info', base))).status, 403);
  assert.equal((await post('/api/jobs', { command: 'devices' }, { 'Content-Type': 'application/json' })).status, 403);
  assert.equal((await post('/api/jobs', { command: 'devices' }, { ...H, Origin: 'http://evil.example' })).status, 403);
  const port = new URL(base).port;
  const status = await new Promise((resolve) => {
    http.get({ host: '127.0.0.1', port, path: '/api/info', headers: { Host: `evil.example:${port}`, 'X-ByOS-Token': 'testtoken' } },
      (res) => { res.resume(); resolve(res.statusCode); });
  });
  assert.equal(status, 421);
  const info = await (await fetch(new URL('/api/info', base), { headers: H })).json();
  assert.equal(info.mock, true);
});

test('devices and ls run through byos-cli and stream back', async () => {
  const dev = await post('/api/jobs', { command: 'devices', params: { wait: 0 } });
  assert.equal(dev.status, 202);
  const d = await events(dev.json.id);
  assert.equal(d.final.ok, true);
  assert.equal(d.events.filter((e) => e.event === 'device').length, 2);

  const ls = await post('/api/jobs', { command: 'ls', params: { path: 'Music/ABBA/Arrival', device: 'usb:MOCK0001' } });
  const l = await events(ls.json.id);
  assert.equal(l.final.ok, true);
  assert.deepEqual(l.events.filter((e) => e.event === 'entry').map((e) => e.name),
    ['01 When I Kissed the Teacher.flac', '02 Dancing Queen.flac', 'cover.jpg']);

  // Replay after the end still works (late reader).
  const again = await events(ls.json.id);
  assert.equal(again.final.ok, true);
});

test('bad requests are refused before anything runs', async () => {
  assert.equal((await post('/api/jobs', { command: 'patch' })).status, 400);
  assert.equal((await post('/api/jobs', { command: 'ls', params: { path: '../x' } })).status, 400);
  assert.equal((await post('/api/jobs', { command: 'put', params: {} })).status, 400);
});

test('upload stores the file, runs put and cleans up', async () => {
  const q = new URLSearchParams({ dir: 'Music/Uploads', name: 'Canción.flac', device: 'usb' });
  const r = await fetch(new URL(`/api/upload?${q}`, base), {
    method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: Buffer.alloc(12345, 7),
  });
  assert.equal(r.status, 202);
  const { id } = await r.json();
  const e = await events(id);
  assert.equal(e.final.ok, true);
  assert.equal(e.final.written, 1);
  assert.equal(e.final.bytes, 12345);
  const ls = await post('/api/jobs', { command: 'ls', params: { path: 'Music/Uploads' } });
  const l = await events(ls.json.id);
  assert.deepEqual(l.events.filter((x) => x.event === 'entry').map((x) => [x.name, x.size]), [['Canción.flac', 12345]]);

  const bad = new URLSearchParams({ dir: 'Music', name: 'what?.flac' });
  assert.equal((await fetch(new URL(`/api/upload?${bad}`, base), { method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: 'x' })).status, 400);
  const root = new URLSearchParams({ dir: '', name: 'a.flac' });
  assert.equal((await fetch(new URL(`/api/upload?${root}`, base), { method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: 'x' })).status, 400);
});

test('a prompt can be answered over the API', async () => {
  const job = await post('/api/jobs', { command: 'rm', params: { paths: ['Music/Daft Punk'], recursive: true } });
  const id = job.json.id;
  // Wait for the prompt, then answer it.
  for (let i = 0; i < 100; i++) {
    const s = await (await fetch(new URL(`/api/jobs/${id}`, base), { headers: H })).json();
    if (s.events.some((e) => e.event === 'prompt')) break;
    await new Promise((r) => setTimeout(r, 30));
  }
  assert.equal((await post(`/api/jobs/${id}/answer`, { answer: true })).status, 200);
  const e = await events(id);
  assert.deepEqual(e.final.deleted, ['Music/Daft Punk']);
});

test('an upload keeps the original modification time', async () => {
  const mtime = 1600000000000;
  const q = new URLSearchParams({ dir: 'Music/Dated', name: 'old.mp3', device: 'usb', mtime: String(mtime) });
  const r = await fetch(new URL(`/api/upload?${q}`, base), {
    method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: Buffer.alloc(100, 1),
  });
  assert.equal(r.status, 202);
  assert.equal((await events((await r.json()).id)).final.ok, true);
  const l = await events((await post('/api/jobs', { command: 'ls', params: { path: 'Music/Dated' } })).json.id);
  assert.equal(l.events.find((x) => x.event === 'entry').mtime, mtime / 1000);
  const bad = new URLSearchParams({ dir: 'Music', name: 'a.mp3', mtime: '-5' });
  assert.equal((await fetch(new URL(`/api/upload?${bad}`, base), { method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: 'x' })).status, 400);
});

function ourTempFolders() {
  return readdirSync(os.tmpdir()).filter((n) => n.startsWith(`byos-desktop-${process.pid}-`));
}

test('an upload refused because byos-cli is missing leaves nothing in the temp folder', async () => {
  const missing = createApp({ cli: path.join(tmp, 'nope.jar'), token: 't2' });
  const at = await missing.listen(0);
  try {
    const before = ourTempFolders().length;
    const q = new URLSearchParams({ dir: 'Music', name: 'a.mp3' });
    const r = await fetch(new URL(`/api/upload?${q}`, at), { method: 'POST', headers: { 'X-ByOS-Token': 't2' }, body: Buffer.alloc(5000) });
    assert.equal(r.status, 503);
    assert.equal(ourTempFolders().length, before);
  } finally {
    await missing.close();
  }
});

test('jobs queue per device; discovery and other devices are not held up', async () => {
  const slowEnv = { ...env, BYOS_MOCK_STATE: path.join(tmp, 'state-q.json'), BYOS_MOCK_DELAY_MS: '1500' };
  const q = createApp({ mock: true, env: slowEnv, token: 't3', jobKeepMs: 100 });
  const at = await q.listen(0);
  const P = (body) => fetch(new URL('/api/jobs', at), {
    method: 'POST', headers: { 'X-ByOS-Token': 't3', 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  }).then((r) => r.json());
  try {
    const a1 = await P({ command: 'ls', params: { path: 'Music', device: 'usb:AAA' } });
    const a2 = await P({ command: 'ls', params: { path: 'Music', device: 'usb:AAA' } });
    const b1 = await P({ command: 'ls', params: { path: 'Music', device: 'usb:BBB' } });
    const d = await P({ command: 'devices', params: { wait: 0 } });
    assert.equal(q.jobs.get(a1.id).state, 'running');
    assert.equal(q.jobs.get(a2.id).state, 'queued');
    assert.equal(q.jobs.get(b1.id).state, 'running');
    assert.equal(q.jobs.get(d.id).state, 'running');
    // Cancelling the queued job ends it, and it is forgotten later like any other job.
    const c = await fetch(new URL(`/api/jobs/${a2.id}/cancel`, at), { method: 'POST', headers: { 'X-ByOS-Token': 't3' } });
    assert.equal(c.status, 200);
    assert.equal(q.jobs.get(a2.id).final.code, 'cancelled');
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(q.jobs.has(a2.id), false);
    assert.equal((await events(a1.id, at, 't3')).final.ok, true);
  } finally {
    await q.close();
  }
});

test('closing the runner ends queued and running jobs and deletes their uploads', async () => {
  const slowEnv = { ...env, BYOS_MOCK_STATE: path.join(tmp, 'state-c.json'), BYOS_MOCK_SCENARIO: 'slow', BYOS_MOCK_DELAY_MS: '3000' };
  const c = createApp({ mock: true, env: slowEnv, token: 't4', killGraceMs: 500 });
  const at = await c.listen(0);
  const before = ourTempFolders().length;
  const q = new URLSearchParams({ dir: 'Music/Up', name: 'big.flac', device: 'usb' });
  const up = await (await fetch(new URL(`/api/upload?${q}`, at), {
    method: 'POST', headers: { 'X-ByOS-Token': 't4' }, body: Buffer.alloc(50000),
  })).json();
  const queued = await (await fetch(new URL('/api/jobs', at), {
    method: 'POST', headers: { 'X-ByOS-Token': 't4', 'Content-Type': 'application/json' },
    body: JSON.stringify({ command: 'ls', params: { device: 'usb' } }),
  })).json();
  assert.equal(c.jobs.get(queued.id).state, 'queued');
  assert.equal(ourTempFolders().length, before + 1);
  await c.close();
  assert.equal(c.jobs.get(up.id).state, 'done');
  assert.equal(c.jobs.get(up.id).final.ok, false);
  assert.equal(c.jobs.get(queued.id).final.code, 'cancelled');
  assert.equal(ourTempFolders().length, before);
});

test('stale upload folders of dead runners are removed at startup', () => {
  const dir = mkdtempSync(path.join(tmp, 'sweep-'));
  const dead = path.join(dir, 'byos-desktop-4194303-abcdef'); // no such process
  const live = path.join(dir, `byos-desktop-${process.pid}-abcdef`);
  const other = path.join(dir, 'something-else');
  for (const d of [dead, live, other]) mkdirSync(d);
  assert.equal(sweepStaleTemp(dir), 1);
  assert.equal(existsSync(dead), false);
  assert.equal(existsSync(live), true);
  assert.equal(existsSync(other), true);
});
