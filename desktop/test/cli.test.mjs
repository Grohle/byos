import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { buildArgs, Job, OP_TIMEOUTS_MS, PROMPT_TIMEOUT_MS, resolveCli, runJob, UsageError } from '../lib/cli.mjs';

const tmp = mkdtempSync(path.join(os.tmpdir(), 'byos-cli-test-'));
test.after(() => rmSync(tmp, { recursive: true, force: true }));

let n = 0;
function mockEnv(extra = {}) {
  return { ...process.env, BYOS_MOCK_STATE: path.join(tmp, `state-${n++}.json`), ...extra };
}
const mock = resolveCli({ mock: true });

test('buildArgs builds the documented command lines', () => {
  assert.deepEqual(buildArgs('devices', {}), ['devices', '--wait', '1500']);
  assert.deepEqual(buildArgs('ls', { path: 'Music', all: true, device: 'usb:ABC123' }, { config: 'C:\\cfg' }),
    ['--config', 'C:\\cfg', '--device', 'usb:ABC123', 'ls', '--all', '--', 'Music']);
  assert.deepEqual(buildArgs('ls', {}), ['ls']);
  assert.deepEqual(buildArgs('rm', { paths: ['a', 'b/c'], recursive: true, yes: true }), ['rm', '-r', '--yes', '--', 'a', 'b/c']);
  assert.deepEqual(buildArgs('mkdir', { paths: ['New'], parents: true }), ['mkdir', '-p', '--', 'New']);
  assert.deepEqual(buildArgs('mv', { src: 'a', dst: 'b' }), ['mv', '--', 'a', 'b']);
  assert.deepEqual(buildArgs('connect', { pair: true, code: '815365', device: 'wifi' }),
    ['--device', 'wifi', 'connect', '--pair', '--code', '815365']);
  const local = path.join(tmp, 'x.flac');
  assert.deepEqual(buildArgs('put', { local: [local], dir: 'Music' }),
    ['put', '--mkdir', '--skip-identical', '--', local, 'Music/']);
  assert.deepEqual(buildArgs('status', { start: true }), ['status', '--start']);
});

test('names that look like options stay paths (after "--")', () => {
  assert.deepEqual(buildArgs('rm', { paths: ['-r', '--yes'] }), ['rm', '--', '-r', '--yes']);
  assert.deepEqual(buildArgs('mv', { src: '--replace', dst: 'x' }), ['mv', '--', '--replace', 'x']);
  assert.deepEqual(buildArgs('ls', { path: '--all' }), ['ls', '--', '--all']);
  const local = path.join(tmp, 'y.mp3');
  assert.deepEqual(buildArgs('put', { local: [local], dir: '--recursive', mode: 'create' }),
    ['put', '--mkdir', '--', local, '--recursive/']);
  // Every flag comes before the marker.
  for (const args of [buildArgs('rm', { paths: ['-x'], recursive: true, yes: true }),
    buildArgs('mkdir', { paths: ['--p'], parents: true })]) {
    const i = args.indexOf('--');
    assert.ok(i > 0);
    assert.ok(args.slice(0, i).every((a, k) => k === 0 || a.startsWith('-')));
  }
});

test('the mock CLI honours "--": an item named like an option is handled as a name', async () => {
  const env = mockEnv();
  for (const name of ['-r', '--yes', '--overwrite']) {
    assert.equal((await runJob(mock, buildArgs('mkdir', { paths: [`Music/${name}`] }), { env })).final.ok, true, name);
  }
  assert.equal((await runJob(mock, buildArgs('mkdir', { paths: ['--all'] }), { env })).final.ok, true);
  const ls = await runJob(mock, buildArgs('ls', { path: '--all' }), { env });
  assert.equal(ls.final.ok, true);
  assert.equal(ls.final.path, '--all');
  const mv = await runJob(mock, buildArgs('mv', { src: 'Music/--yes', dst: 'Music/-yes2' }), { env });
  assert.equal(mv.final.ok, true);
  // Without -r an empty folder named "-r" is deleted, nothing else (and no prompt).
  const rm = await runJob(mock, buildArgs('rm', { paths: ['Music/-r'] }), { env });
  assert.deepEqual(rm.final.deleted, ['Music/-r']);
  const names = (await runJob(mock, buildArgs('ls', { path: 'Music' }), { env })).events
    .filter((e) => e.event === 'entry').map((e) => e.name);
  assert.ok(names.includes('ABBA') && names.includes('-yes2') && names.includes('--overwrite') && !names.includes('-r'));
});

test('buildArgs normalises paths to NFC', () => {
  const nfd = 'N\u0303andu\u0301';
  assert.deepEqual(buildArgs('ls', { path: nfd }), ['ls', '--', 'Ñandú']);
});

test('buildArgs refuses what the UI must never send', () => {
  const bad = [
    ['patch', {}],
    ['ls', { path: '../etc' }],
    ['ls', { path: '/abs' }],
    ['rm', { paths: [] }],
    ['rm', { paths: ['.hidden'] }],
    ['mv', { src: 'a', dst: 'b?' }],
    ['connect', { code: '12ab56' }],
    ['ls', { device: 'usb; rm -rf /' }],
    ['ls', { device: '--adb' }],
    ['devices', { wait: 999999 }],
    ['put', { local: [path.join(tmp, 'x')], dir: '' }],
    ['put', { local: ['relative.flac'], dir: 'Music' }],
  ];
  for (const [cmd, params] of bad) {
    assert.throws(() => buildArgs(cmd, params), UsageError, `${cmd} ${JSON.stringify(params)}`);
  }
});

test('resolveCli explains a missing jar', () => {
  const r = resolveCli({ cli: path.join(tmp, 'nope.jar') });
  assert.match(r.error, /byos-cli not found/);
  const j = resolveCli({ env: { BYOS_CLI: path.join(tmp, 'nope.jar') } });
  assert.match(j.error, /nope\.jar/);
});

test('devices: one start, device lines, one final result', async () => {
  const { final, events, exitCode } = await runJob(mock, ['devices', '--wait', '0'], { env: mockEnv() });
  assert.equal(exitCode, 0);
  assert.equal(final.ok, true);
  assert.equal(final.count, 2);
  assert.equal(events[0].event, 'start');
  assert.equal(events.filter((e) => e.event === 'device').length, 2);
  assert.equal(events.filter((e) => e.event === 'result' || e.event === 'error').length, 1);
  assert.equal(events.at(-1), final);
});

test('ls streams entries and errors map to exit codes', async () => {
  const env = mockEnv();
  const ok = await runJob(mock, ['ls', 'Music'], { env });
  assert.deepEqual(ok.events.filter((e) => e.event === 'entry').map((e) => e.name), ['ABBA', 'Daft Punk', 'Ñandú']);
  const missing = await runJob(mock, ['ls', 'Nope'], { env });
  assert.equal(missing.exitCode, 4);
  assert.equal(missing.final.code, 'remote');
  assert.equal(missing.final.btp.name, 'E_NOT_FOUND');
  const usb = await runJob(mock, ['ls', ''], { env: { ...env, BYOS_MOCK_SCENARIO: 'usb_storage' } });
  assert.equal(usb.final.btp.name, 'E_SD_UNAVAILABLE');
});

test('mkdir, mv and rm change the fake card', async () => {
  const env = mockEnv();
  assert.equal((await runJob(mock, ['mkdir', 'Music/New'], { env })).final.ok, true);
  assert.equal((await runJob(mock, ['mkdir', 'Music/New'], { env })).final.btp.name, 'E_EXISTS');
  assert.equal((await runJob(mock, ['mv', 'Music/New', 'Music/Renamed'], { env })).final.ok, true);
  const ls = await runJob(mock, ['ls', 'Music'], { env });
  assert.ok(ls.events.some((e) => e.event === 'entry' && e.name === 'Renamed'));
  assert.equal((await runJob(mock, ['rm', 'Music/ABBA'], { env })).final.btp.name, 'E_NOT_EMPTY');
  assert.deepEqual((await runJob(mock, ['rm', 'Music/ABBA', '-r', '--yes'], { env })).final.deleted, ['Music/ABBA']);
});

test('a prompt is answered through stdin', async () => {
  const env = mockEnv();
  const job = new Job(mock, ['rm', 'Music/ABBA', '-r'], { env });
  const done = new Promise((r) => job.on('end', r));
  job.on('event', (ev) => { if (ev.event === 'prompt') job.answer(true); });
  job.start();
  const final = await done;
  assert.equal(final.ok, true);
  assert.deepEqual(final.deleted, ['Music/ABBA']);
});

test('pairing with the right and the wrong code', async () => {
  for (const [code, ok] of [['815365', true], ['000000', false]]) {
    const job = new Job(mock, ['--device', 'wifi', 'connect', '--pair'], { env: mockEnv() });
    const done = new Promise((r) => job.on('end', r));
    job.on('event', (ev) => { if (ev.event === 'prompt' && ev.kind === 'pair_code') job.answer(code); });
    job.start();
    const final = await done;
    assert.equal(final.ok, ok, code);
    if (!ok) assert.equal(final.code, 'auth');
    else assert.equal(final.encrypted, true);
  }
});

test('cancel stops a slow upload with code cancelled', async () => {
  const env = mockEnv({ BYOS_MOCK_SCENARIO: 'slow' });
  const local = path.join(tmp, 'song.flac');
  writeFileSync(local, Buffer.alloc(100000));
  const job = new Job(mock, ['put', local, 'Music/Up/', '--mkdir'], { env });
  const done = new Promise((r) => job.on('end', r));
  job.on('event', (ev) => { if (ev.event === 'progress' && ev.op === 'put') job.cancel(); });
  job.start();
  const final = await done;
  assert.equal(final.ok, false);
  assert.equal(final.code, 'cancelled');
  assert.equal(job.exitCode, 7);
});

test('a CLI that exits without a final line is reported as internal', async () => {
  const script = path.join(tmp, 'broken.mjs');
  writeFileSync(script, 'console.log(JSON.stringify({v:1,event:"start",t:0}));console.log("garbage");console.error("boom");process.exit(3);');
  const r = resolveCli({ cli: script });
  const { final, events, exitCode } = await runJob(r, []);
  assert.equal(exitCode, 3);
  assert.equal(final.code, 'internal');
  assert.match(final.stderr, /boom/);
  assert.ok(events.some((e) => e.event === 'warn' && e.code === 'bad_output'));
});

test('a silent CLI is stopped by the watchdog and reported as timeout', async () => {
  const script = path.join(tmp, 'silent.mjs');
  writeFileSync(script, 'setTimeout(() => {}, 60000);');
  const r = resolveCli({ cli: script });
  const { final } = await runJob(r, [], { timeoutMs: 300, killGraceMs: 200 });
  assert.equal(final.code, 'timeout');
});

test('the default limits cover the documented long waits', () => {
  assert.ok(OP_TIMEOUTS_MS.pair_wait >= 130000);
  assert.ok(OP_TIMEOUTS_MS.install >= 70000 && OP_TIMEOUTS_MS.reconnect >= 70000);
  assert.ok(PROMPT_TIMEOUT_MS >= 130000);
  const job = new Job(mock, []);
  assert.equal(job.currentLimit(), 25000);
  job.lastOp = 'pair_wait';
  assert.equal(job.currentLimit(), 130000);
  job.lastOp = 'put';
  assert.equal(job.currentLimit(), 25000);
  job.waitingForAnswer = true;
  assert.equal(job.currentLimit(), PROMPT_TIMEOUT_MS);
});

// Pairing where the user takes longer than the normal silence limit to tap Allow.
async function slowAllow(scenario, opTimeoutsMs) {
  const env = mockEnv({ BYOS_MOCK_SCENARIO: scenario, BYOS_MOCK_PAIR_WAIT_MS: '1200', BYOS_MOCK_HEARTBEAT_MS: '150' });
  const job = new Job(mock, ['--device', 'wifi', 'connect', '--pair'], { env, timeoutMs: 400, opTimeoutsMs, killGraceMs: 300 });
  const done = new Promise((r) => job.on('end', r));
  job.on('event', (ev) => { if (ev.event === 'prompt') job.answer('815365'); });
  job.start();
  return { final: await done, job };
}

test('pair_wait: heartbeats keep a long wait alive', async () => {
  const { final, job } = await slowAllow('ok', {});
  assert.equal(final.ok, true);
  assert.ok(job.events.filter((e) => e.op === 'pair_wait').length >= 5);
});

test('pair_wait without heartbeats: the longer per-operation limit applies', async () => {
  const { final, job } = await slowAllow('silent_pair', { pair_wait: 3000 });
  assert.equal(final.ok, true);
  assert.equal(job.events.filter((e) => e.op === 'pair_wait').length, 1);
  // With only the normal limit the same run is cut short: this is what the review found.
  const cut = await slowAllow('silent_pair', {});
  assert.equal(cut.final.code, 'timeout');
});

test('an unanswered prompt is answered "no" and ends as a prompt timeout', async () => {
  const job = new Job(mock, ['--device', 'wifi', 'connect', '--pair'], { env: mockEnv(), promptTimeoutMs: 300, killGraceMs: 500 });
  const final = await new Promise((r) => { job.on('end', r); job.start(); });
  assert.equal(final.ok, false);
  assert.equal(final.code, 'timeout');
  assert.equal(final.reason, 'prompt');
  assert.ok(job.events.some((e) => e.event === 'warn' && e.code === 'prompt_timeout'));
});

test('a missing program gives a clear error', async () => {
  const { final } = await runJob({ program: path.join(tmp, 'no-such-program.exe'), prefix: [] }, []);
  assert.equal(final.ok, false);
  assert.ok(['no_cli', 'internal'].includes(final.code));
});

test('a USB serial with a space is a valid device (HiBy R3 II 2025)', () => {
  assert.deepEqual(buildArgs('status', { device: 'usb:HiBy R3II_2025' }), ['--device', 'usb:HiBy R3II_2025', 'status']);
  assert.throws(() => buildArgs('status', { device: 'usb: lead' }));
  assert.throws(() => buildArgs('status', { device: 'usb:a;b' }));
});
