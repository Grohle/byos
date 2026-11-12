// The "Build ByOS and install it" contract: argument building, the JSON lines of the build
// commands (docs/BYOS_CLI.md, "Build ByOS") as the mock prints them, and lib/build-flow.mjs,
// which turns them into the Updates page's four steps.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { buildArgs, Job, resolveCli, UsageError } from '../lib/cli.mjs';
import {
  applyEvent, applyInfo, BUILD_PHASES, emptyFlow, finishStep, fraction, pending, startStep, STEPS,
} from '../lib/build-flow.mjs';
import { createApp } from '../server.mjs';

const tmp = mkdtempSync(path.join(os.tmpdir(), 'byos-build-test-'));
test.after(() => rmSync(tmp, { recursive: true, force: true }));
let n = 0;
const mock = resolveCli({ mock: true });
function env(extra = {}) {
  return { ...process.env, BYOS_MOCK_STATE: path.join(tmp, `state-${n++}.json`), BYOS_MOCK_STEP_MS: '5', ...extra };
}

// Runs one mock command and feeds its lines into the flow, like the page does.
function run(args, e, flow, step) {
  return new Promise((resolve) => {
    const job = new Job(mock, args, { env: e });
    if (flow) startStep(flow, step);
    job.on('event', (ev) => { if (flow) applyEvent(flow, step, ev); });
    job.on('end', (final) => {
      if (flow) finishStep(flow, step, final);
      resolve({ final, events: job.events, exitCode: job.exitCode });
    });
    job.start();
  });
}

test('buildArgs: the build commands, with nothing the page could steer', () => {
  assert.deepEqual(buildArgs('build-info', {}), ['build-info']);
  assert.deepEqual(buildArgs('bundle-update', { check: true }), ['bundle-update', '--check']);
  assert.deepEqual(buildArgs('firmware-fetch', {}), ['firmware-fetch']);
  const picked = path.join(tmp, 'firmware.upt');
  assert.deepEqual(buildArgs('firmware-fetch', { from: picked }), ['firmware-fetch', '--from', picked]);
  assert.deepEqual(buildArgs('build', { force: true }), ['build', '--force']);
  assert.deepEqual(buildArgs('build-send', { device: 'usb:ABC', install: true, yes: true }),
    ['--device', 'usb:ABC', 'build-send', '--install', '--yes']);
  assert.deepEqual(buildArgs('build-send', { device: 'usb', noWait: true, install: true }),
    ['--device', 'usb', 'build-send', '--install', '--no-wait']);
  for (const [cmd, params] of [
    ['build-send', { device: 'wifi:abcd', install: true }],
    ['build-send', { device: '192.168.1.20' }],
    ['firmware-fetch', { from: 'relative.upt' }],
    ['firmware-fetch', { from: 42 }],
    ['patch', {}],
  ]) {
    assert.throws(() => buildArgs(cmd, params), UsageError, `${cmd} ${JSON.stringify(params)}`);
  }
});

test('the whole flow over the mock: download, build, copy, install', async () => {
  const e = env();
  const flow = emptyFlow();
  const info = await run(['build-info'], e);
  assert.equal(info.final.firmware_ready, false);
  assert.equal(info.final.build, null);
  assert.equal(info.final.firmware.file, 'r3ii_2025.upt');
  applyInfo(flow, info.final);
  assert.deepEqual(pending(flow), STEPS);

  const fw = await run(['firmware-fetch'], e, flow, 'firmware');
  assert.equal(fw.final.ok, true);
  assert.equal(fw.final.source, 'download');
  assert.ok(fw.events.some((x) => x.op === 'download' && x.unit === 'bytes' && x.total === 46759936));
  assert.ok(fw.events.some((x) => x.op === 'check' && x.unit === 'permille'));
  assert.equal(flow.steps.firmware.status, 'done');

  const seen = [];
  const b = await run(['build'], e, flow, 'build');
  for (const ev of b.events) if (ev.op === 'patch') seen.push(ev.stage);
  assert.equal(b.final.ok, true);
  assert.equal(b.final.cached, false);
  // the stages arrive in the patcher's order, every one a known name
  assert.deepEqual([...new Set(seen)], BUILD_PHASES.filter((p) => p !== 'export'));
  assert.ok(b.events.every((ev) => ev.op !== 'patch' || (ev.unit === 'permille' && ev.done <= 1000)));
  assert.equal(flow.steps.build.status, 'done');
  assert.equal(flow.sendTotal, b.final.size + b.final.sd_size);

  // build-info now knows both, so a new page starts at "copy"
  const again = applyInfo(emptyFlow(), (await run(['build-info'], e)).final);
  assert.deepEqual(pending(again), ['send', 'install']);
  assert.equal((await run(['build'], e)).final.cached, true);

  const fractions = [];
  const sendFlow = flow;
  const s = await new Promise((resolve) => {
    const job = new Job(mock, ['--device', 'usb:MOCK0001', 'build-send', '--install', '--yes'], { env: e });
    startStep(sendFlow, 'send');
    job.on('event', (ev) => {
      applyEvent(sendFlow, 'send', ev);
      const f = fraction(sendFlow.steps.send);
      if (f !== null) fractions.push(f);
      if (ev.op === 'reboot') assert.equal(sendFlow.steps.install.status, 'active');
    });
    job.on('end', (final) => { finishStep(sendFlow, 'send', final); resolve({ final, events: job.events }); });
    job.start();
  });
  assert.equal(s.final.ok, true);
  assert.equal(s.final.state, 'installed');
  assert.equal(s.final.version, '0.9.0');
  // copy progress only moves forward and ends full
  assert.ok(fractions.length > 3);
  assert.ok(fractions.every((f, i) => i === 0 || f >= fractions[i - 1]));
  assert.ok(Math.abs(fractions.at(-1) - 1) < 1e-9);
  // the firmware is copied last
  const puts = s.events.filter((x) => x.op === 'put').map((x) => x.path);
  assert.equal(puts.at(-1), 'r3ii_2025.upt');
  assert.ok(puts.indexOf('ByOS/payload/VERSION') > puts.indexOf('ByOS/payload/MANIFEST.md5'));
  assert.equal(flow.phase, 'done');
  assert.deepEqual(pending(flow), []);
  // the HiBy now reports the new version
  const st = await run(['status'], e);
  assert.equal(st.final.device.byos_version, '0.9.0');
});

test('a firmware file the user picked: accepted, or refused as the wrong file', async () => {
  const e = env();
  const small = path.join(tmp, 'notes.upt');
  writeFileSync(small, 'not a firmware');
  const flow = emptyFlow();
  const bad = await run(['firmware-fetch', '--from', small], e, flow, 'firmware');
  assert.equal(bad.exitCode, 6);
  assert.equal(bad.final.code, 'verify');
  assert.match(bad.final.md5, /^[0-9a-f]{32}$/);
  assert.equal(flow.steps.firmware.status, 'failed');
  assert.equal(flow.errorStep, 'firmware');
  const big = path.join(tmp, 'r3ii_2025.upt');
  writeFileSync(big, Buffer.alloc(2 * 1024 * 1024));
  const ok = await run(['firmware-fetch', '--from', big], e, flow, 'firmware');
  assert.equal(ok.final.ok, true);
  assert.equal(ok.final.source, 'file');
  assert.equal(flow.steps.firmware.status, 'done');
  assert.equal(flow.steps.firmware.detail, 'file');
});

test('failures land on the right step', async () => {
  // no internet: the firmware step fails and names HiBy's page
  const off = emptyFlow();
  const r1 = await run(['firmware-fetch'], env({ BYOS_MOCK_SCENARIO: 'offline' }), off, 'firmware');
  assert.equal(r1.final.code, 'no_device');
  assert.match(r1.final.page, /^https:\/\//);
  assert.equal(off.errorStep, 'firmware');

  // build without firmware, and a build that fails its own check
  const e = env({ BYOS_MOCK_SCENARIO: 'build_fail' });
  assert.equal((await run(['build'], e)).final.code, 'usage');
  await run(['firmware-fetch'], e);
  const bf = emptyFlow();
  const r2 = await run(['build'], e, bf, 'build');
  assert.equal(r2.exitCode, 6);
  assert.equal(bf.steps.build.status, 'failed');

  // a low battery stops before the restart: the install step fails, not the copy
  const lb = env({ BYOS_MOCK_SCENARIO: 'low_battery' });
  await run(['firmware-fetch'], lb);
  await run(['build'], lb);
  const f3 = emptyFlow();
  const r3 = await run(['--device', 'usb', 'build-send', '--install', '--yes'], lb, f3, 'send');
  assert.equal(r3.final.btp.name, 'E_BATTERY');
  assert.equal(f3.errorStep, 'install');
  assert.equal(f3.steps.send.status, 'done');

  // the HiBy's own checks refuse after it was asked: reported as an install failure
  const rr = env({ BYOS_MOCK_SCENARIO: 'reinstall_refused' });
  await run(['firmware-fetch'], rr);
  await run(['build'], rr);
  const f4 = emptyFlow();
  startStep(f4, 'send');
  applyEvent(f4, 'send', { event: 'progress', op: 'reboot', done: 0, total: 0, unit: 'none' });
  finishStep(f4, 'send', (await run(['--device', 'usb', 'build-send', '--install', '--yes'], rr)).final);
  assert.equal(f4.errorStep, 'install');
  assert.equal(f4.steps.send.status, 'done');

  // cancelling is not a failure of the step
  const f5 = emptyFlow();
  startStep(f5, 'build');
  finishStep(f5, 'build', { ok: false, code: 'cancelled', message: 'cancelled' });
  assert.equal(f5.steps.build.status, 'todo');
});

test('a confirmation prompt is answered "no": nothing is installed', async () => {
  const e = env();
  await run(['firmware-fetch'], e);
  await run(['build'], e);
  const job = new Job(mock, ['--device', 'usb', 'build-send', '--install'], { env: e });
  const final = await new Promise((resolve) => {
    job.on('event', (ev) => { if (ev.event === 'prompt') job.answer(false); });
    job.on('end', resolve);
    job.start();
  });
  assert.equal(final.code, 'cancelled');
  assert.equal((await run(['status'], e)).final.device.byos_version, '0.8.0');
});

test('build-flow ignores what it does not know', () => {
  const f = emptyFlow();
  applyEvent(f, 'build', null);
  applyEvent(f, 'build', { event: 'progress', op: 'patch', stage: 'bogus', done: -5, total: 1000 });
  assert.equal(f.steps.build.phase, 'check');
  assert.equal(f.steps.build.done, 0);
  applyEvent(f, 'send', { event: 'warn', code: 'parked', path: 'update.upt' });
  applyEvent(f, 'send', { event: 'warn', code: 'parked', path: 'update.upt' });
  assert.deepEqual(f.parked, ['update.upt']);
  assert.equal(fraction(f.steps.install), null);
  assert.equal(applyInfo(f, { ok: false }), f);
});

// ---- the runner -----------------------------------------------------------------------------

test('runner: a picked firmware file goes through /api/firmware only, and the temp copy is removed', async () => {
  const state = path.join(tmp, 'server-state.json');
  const app = createApp({ mock: true, env: { ...process.env, BYOS_MOCK_STATE: state, BYOS_MOCK_STEP_MS: '5' }, token: 'tok' });
  const base = await app.listen(0);
  const H = { 'X-ByOS-Token': 'tok', 'Content-Type': 'application/json' };
  try {
    // never with a path from the page
    const direct = await fetch(new URL('/api/jobs', base), {
      method: 'POST', headers: H, body: JSON.stringify({ command: 'firmware-fetch', params: { from: path.join(tmp, 'x.upt') } }),
    });
    assert.equal(direct.status, 400);
    const up = await fetch(new URL('/api/firmware', base), {
      method: 'POST', headers: { 'X-ByOS-Token': 'tok' }, body: Buffer.alloc(1536 * 1024),
    });
    assert.equal(up.status, 202);
    const { id } = await up.json();
    const job = app.jobs.get(id);
    const final = job.final ?? await new Promise((r) => job.once('end', r));
    assert.equal(final.ok, true);
    assert.equal(final.source, 'file');
    const from = job.args[job.args.indexOf('--from') + 1];
    await new Promise((r) => setTimeout(r, 50));
    assert.equal((await import('node:fs')).existsSync(path.dirname(from)), false);
    // a build does not wait behind a device's queue, and build-info answers at once
    const b = await fetch(new URL('/api/jobs', base), { method: 'POST', headers: H, body: JSON.stringify({ command: 'build' }) });
    const bj = app.jobs.get((await b.json()).id);
    assert.equal(bj.spec, '#local');
    const info = await fetch(new URL('/api/jobs', base), { method: 'POST', headers: H, body: JSON.stringify({ command: 'build-info' }) });
    assert.notEqual(app.jobs.get((await info.json()).id).state, 'queued');
    await new Promise((r) => (bj.final ? r() : bj.once('end', r)));
    // the launch address keeps ?lang= and ?demo= across its redirect
    const r = await fetch(new URL('/?t=tok&demo=building&lang=es', base), { redirect: 'manual' });
    assert.equal(r.status, 303);
    assert.equal(r.headers.get('location'), '/?demo=building&lang=es');
    // the page gets the flow module
    const mod = await fetch(new URL('/build-flow.js', base), { headers: { Cookie: `byos_${new URL(base).port}=tok` } });
    assert.match(await mod.text(), /export function applyEvent/);
  } finally {
    await app.close();
  }
});
