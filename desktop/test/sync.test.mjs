// The Sync tab end to end: the runner's routes and checks (the page never names a file on this
// computer), the link commands, the Music tab's migration, and every way a sync can end, against
// the fake byos-cli (mock/sync-mock.mjs) with its scenarios. Also the Sync tab's translations.

import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { buildArgs, UsageError } from '../lib/cli.mjs';
import {
  decisionsFor, deletionSummary, guards, initialChoices, lockView, migrationPlan, outcome, pendingRun, setGroup,
} from '../lib/sync-flow.mjs';
import { checkDecisions, createApp } from '../server.mjs';
import { STRINGS } from '../ui/i18n.js';

const tmp = mkdtempSync(path.join(os.tmpdir(), 'byos-sync-test-'));
const music = path.join(tmp, 'My Music');
const books = path.join(tmp, 'My Books');
mkdirSync(music, { recursive: true });
mkdirSync(books, { recursive: true });
const DEV = '0123456789abcdef';

// ---- argument checks --------------------------------------------------------------------------

test('link and folder sync commands: arguments are checked', () => {
  assert.deepEqual(buildArgs('link-ls', { deviceId: DEV }), ['link-ls', '--device-id', DEV]);
  assert.throws(() => buildArgs('link-ls', { deviceId: '../x' }), UsageError);
  assert.deepEqual(buildArgs('link-add', { deviceId: DEV, kind: 'music', local: music, layout: 'artist-album', playerOnly: 'ask', select: ['artist:ABBA', 'album:ABBA::Gold'], removeUnselected: true }),
    ['link-add', '--device-id', DEV, '--kind', 'music', '--layout', 'artist-album', '--player-only', 'ask', '--select', 'artist:ABBA', '--select', 'album:ABBA::Gold', '--remove-unselected', '--', music]);
  assert.throws(() => buildArgs('link-add', { kind: 'music', local: 'relative' }), UsageError);
  assert.throws(() => buildArgs('link-add', { kind: 'music', local: path.join(tmp, 'missing') }), UsageError);
  assert.throws(() => buildArgs('link-add', { kind: 'podcasts', local: music }), UsageError);
  assert.throws(() => buildArgs('link-add', { kind: 'music', local: music, select: ['--yes'] }), UsageError);
  assert.throws(() => buildArgs('link-add', { kind: 'music', local: music, cardRoot: '../x' }), UsageError);
  assert.deepEqual(buildArgs('link-set', { id: 'a1b2c3d4e5f60718', enabled: false, removeUnselected: false, selectAll: true }),
    ['link-set', '--select-all', '--keep-unselected', '--disable', '--', 'a1b2c3d4e5f60718']);
  assert.throws(() => buildArgs('link-set', { id: '-r' }), UsageError);
  assert.deepEqual(buildArgs('link-rm', { id: 'a1b2c3d4e5f60718' }), ['link-rm', '--', 'a1b2c3d4e5f60718']);
  assert.deepEqual(buildArgs('sync-plan', { kind: 'all', out: path.join(tmp, 'p.json') }), ['sync-plan', '--kind', 'all', '--out', path.join(tmp, 'p.json')]);
  assert.throws(() => buildArgs('sync-plan', { kind: 'everything' }), UsageError);
  assert.deepEqual(buildArgs('sync-apply', { linked: true, plan: path.join(tmp, 'p.json'), decisions: path.join(tmp, 'd.json'), then: 'rescan', yes: true, allowMassDelete: true, takeOver: true }),
    ['sync-apply', '--then', 'rescan', '--decisions', path.join(tmp, 'd.json'), '--yes', '--allow-mass-delete', '--take-over', '--', path.join(tmp, 'p.json')]);
  assert.deepEqual(buildArgs('sync-resume', { then: 'rescan' }), ['sync-resume', '--then', 'rescan']);
  assert.deepEqual(buildArgs('sync-status', { deviceId: DEV }), ['sync-status', '--device-id', DEV]);
  assert.deepEqual(buildArgs('sync-unlock', { force: true }), ['sync-unlock', '--force']);
});

test('decisions: only item ids and known choices', () => {
  assert.deepEqual(checkDecisions({ i1: 'delete', i2: 'both', policy: { music: { player_only: 'ask' } } }),
    { i1: 'delete', i2: 'both', policy: { music: { player_only: 'ask' } } });
  for (const bad of [[], null, { i1: 'format' }, { '../x': 'keep' }, { policy: { music: { player_only: 'wipe' } } }, { policy: { podcasts: { player_only: 'keep' } } }]) {
    assert.throws(() => checkDecisions(bad), UsageError, JSON.stringify(bad));
  }
});

// ---- the runner and the fake byos-cli ------------------------------------------------------------

const H = { 'X-ByOS-Token': 'testtoken', 'Content-Type': 'application/json' };

// One runner per scenario, each with its own folders.
function runner(name, scenario = 'ok', pick = music, step = '0') {
  const dir = path.join(tmp, name);
  const env = { ...process.env, BYOS_MOCK_STATE: path.join(dir, 'state.json'), BYOS_MOCK_SCENARIO: scenario, BYOS_MOCK_SYNC_STEP_MS: step };
  let next = pick;
  const app = createApp({
    mock: true, env, token: 'testtoken', config: path.join(dir, 'cli'), settingsDir: path.join(dir, 'desk'),
    incomingDir: path.join(dir, 'incoming'), pickFolder: async () => next,
  });
  const r = {
    app, base: null,
    pickNext(p) { next = p; },
    async call(method, p, body) {
      const res = await fetch(new URL(p, r.base), { method, headers: H, body: body === undefined ? undefined : JSON.stringify(body) });
      const text = await res.text();
      let json = null;
      try { json = JSON.parse(text); } catch { /* not JSON */ }
      return { status: res.status, json };
    },
    async job(command, params) {
      const s = await r.call('POST', '/api/jobs', { command, params });
      if (s.status !== 202) return { ok: false, status: s.status, error: s.json?.error, code: s.json?.code, events: [] };
      const ev = await fetch(new URL(`/api/jobs/${s.json.id}/events?token=testtoken`, r.base));
      const text = await ev.text();
      const blocks = text.split('\n\n');
      const events = blocks.filter((b) => b.startsWith('data: ')).map((b) => JSON.parse(b.slice(6)));
      const end = blocks.find((b) => b.startsWith('event: end'));
      return { ...JSON.parse(/^data: (.*)$/m.exec(end)[1]), events };
    },
    async link(kind, folderPath = music, extra = {}) {
      r.pickNext(folderPath);
      const p = await r.call('POST', '/api/links/pick', { kind });
      return r.job('link-add', { deviceId: DEV, kind, folder: p.json.folder, ...extra });
    },
  };
  return r;
}

const apps = [];
async function start(name, scenario, pick, step) {
  const r = runner(name, scenario, pick, step);
  r.base = await r.app.listen(0);
  apps.push(r.app);
  return r;
}
test.after(async () => { for (const a of apps) await a.close(); rmSync(tmp, { recursive: true, force: true }); });

test('the page never names a file on this computer', async () => {
  const r = await start('paths');
  // link-add: a path from the page is refused; only a folder picked in this window (by token) works
  assert.equal((await r.job('link-add', { deviceId: DEV, kind: 'music', local: 'C:\\Windows' })).status, 400);
  assert.equal((await r.job('link-add', { deviceId: DEV, kind: 'music', folder: 'C:\\Windows' })).status, 400);
  assert.equal((await r.job('link-add', { deviceId: DEV, kind: 'music', folder: 'f'.repeat(24) })).status, 400);
  const picked = await r.call('POST', '/api/links/pick', { kind: 'music' });
  assert.equal(picked.status, 200);
  assert.match(picked.json.folder, /^[0-9a-f]{24}$/);
  assert.equal(picked.json.name, 'My Music');
  const added = await r.job('link-add', { deviceId: DEV, kind: 'music', folder: picked.json.folder, local: 'C:\\Windows' });
  assert.equal(added.ok, true, JSON.stringify(added));
  assert.equal(added.link.local.path, music);
  // link-set: "local" from the page is ignored
  const set = await r.job('link-set', { deviceId: DEV, id: added.link.id, local: 'C:\\Windows', playerOnly: 'delete' });
  assert.equal(set.ok, true);
  assert.equal(set.link.local.path, music);
  assert.equal(set.link.player_only, 'delete');
  // the plan and the decisions go to the runner's cache, whatever the page says
  const plan = await r.job('sync-plan', { device: 'usb', kind: 'music', out: 'C:\\evil.json', catalog: 'C:\\evil.json' });
  assert.equal(plan.ok, true, JSON.stringify(plan));
  assert.equal(plan.plan, path.join(r.app.cache, 'sync', 'plan-v2.json'));
  assert.equal((await r.job('sync-apply', { device: 'usb', decisions: { i1: 'rm -rf' } })).status, 400);
  assert.equal((await r.job('sync-apply', { device: 'usb', decisions: 'C:\\evil.json' })).status, 400);
  // every path-like parameter is dropped before byos-cli sees it
  for (const command of ['sync-plan', 'sync-apply', 'sync-resume', 'link-ls', 'sync-status', 'music-scan']) {
    const s = await r.call('POST', '/api/jobs', { command, params: { device: 'usb', deviceId: DEV, plan: 'C:\\x.json', local: 'C:\\x', sources: ['C:\\x'], out: 'C:\\x' } });
    if (s.status === 202) {
      const job = r.app.jobs.get(s.json.id);
      assert.ok(!job.args.some((a) => a.startsWith('C:\\x')), `${command}: ${job.args.join(' ')}`);
    }
  }
});

test('migration: the Music tab folders become music links, once', async () => {
  const r = await start('migrate');
  await r.call('POST', '/api/pick-folder', {}); // the Music tab's folder, in settings.json
  const m = await r.call('GET', `/api/migration?device=${DEV}`);
  assert.equal(m.json.migrated, false);
  assert.equal(m.json.sources.length, 1);
  assert.equal((await r.call('GET', '/api/migration?device=..')).status, 400);
  const reqs = migrationPlan(m.json.sources, { all: false, artists: ['aurora fields'], albums: [], genres: ['jazz'] }, { layout: 'artist-album' }, []);
  assert.equal(reqs.length, 1);
  const made = await r.job('link-add', { deviceId: DEV, ...reqs[0] });
  assert.equal(made.ok, true, JSON.stringify(made));
  assert.equal(made.link.selection.mode, 'chosen');
  assert.deepEqual(made.link.selection.artists, ['aurora fields']);
  assert.equal((await r.call('POST', '/api/migration/done', { device: DEV })).status, 200);
  assert.equal((await r.call('GET', `/api/migration?device=${DEV}`)).json.migrated, true);
  // the computer's library reads the linked folders (links.json), not the page's list
  const scan = await r.job('music-scan', { sources: ['C:\\Windows'] });
  assert.equal(scan.ok, true);
  assert.deepEqual(scan.roots, [music]);
});

test('a whole sync: links, plan, preview choices, apply, then nothing left to do', async () => {
  const r = await start('whole');
  assert.equal((await r.link('music')).ok, true);
  assert.equal((await r.link('ebooks', books)).ok, true);
  const ls = await r.job('link-ls', { deviceId: DEV });
  assert.deepEqual(ls.links.map((l) => [l.kind, l.card_root]), [['music', 'Music'], ['ebooks', 'Books']]);
  const status0 = await r.job('sync-status', { deviceId: DEV });
  assert.equal(status0.ok, true);
  assert.deepEqual(status0.runs, []);
  const final = await r.job('sync-plan', { device: 'usb', kind: 'all' });
  assert.equal(final.ok, true);
  assert.deepEqual(final.kinds, ['music', 'ebooks']);
  const plan = (await r.call('GET', '/api/plan')).json;
  assert.equal(plan.format, 2);
  const ch = initialChoices(plan);
  // the user deletes what was added on the HiBy, keeps the rest
  setGroup(ch, plan.items.filter((i) => i.reason === 'added_on_player' && !i.parent), 'delete');
  const del = deletionSummary(plan, ch);
  assert.equal(del.count, 5);
  const apply = await r.job('sync-apply', { device: 'usb', then: 'rescan', yes: true, decisions: decisionsFor(plan, ch), runId: plan.run_id });
  assert.equal(apply.ok, true, JSON.stringify(apply).slice(0, 400));
  assert.deepEqual(outcome(apply), { state: 'done', restart: false });
  assert.equal(apply.kinds.music.removed, 5);
  assert.equal(apply.kinds.music.written, final.summary.music.add + final.summary.music.update);
  assert.equal(apply.kinds.ebooks.written, 6);
  assert.ok(apply.events.some((e) => e.event === 'progress' && e.op === 'library'));
  assert.deepEqual(JSON.parse(readFileSync(path.join(r.app.cache, 'sync', 'decisions.json'), 'utf8')), decisionsFor(plan, ch));
  const again = await r.job('sync-plan', { device: 'usb', kind: 'all' });
  assert.equal(again.items, 0);
  // stop syncing a folder: link-rm
  const rm = await r.job('link-rm', { deviceId: DEV, id: ls.links[1].id });
  assert.equal(rm.ok, true);
  assert.equal(rm.count, 1);
});

test('source_unavailable: the folder is gone, nothing is ticked for deletion', async () => {
  const r = await start('unavailable', 'sync_unavailable');
  await r.link('music');
  const final = await r.job('sync-plan', { device: 'usb', kind: 'music' });
  assert.equal(final.ok, true);
  assert.ok(final.events.some((e) => e.event === 'warn' && e.code === 'source_unavailable'));
  const plan = (await r.call('GET', '/api/plan')).json;
  assert.deepEqual(guards(plan).map((g) => g.reason), ['source_unavailable']);
  assert.equal(deletionSummary(plan, initialChoices(plan)).count, 0);
});

test('mass delete: the typed count (--allow-mass-delete) or nothing is deleted', async () => {
  const r = await start('mass', 'sync_mass_delete');
  await r.link('music');
  await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const plan = (await r.call('GET', '/api/plan')).json;
  assert.deepEqual(guards(plan).map((g) => g.reason), ['mass_delete']);
  const ch = initialChoices(plan);
  assert.equal(deletionSummary(plan, ch).count, 0);
  setGroup(ch, plan.items.filter((i) => i.reason === 'deleted_on_computer' && !i.parent), 'delete');
  assert.equal(deletionSummary(plan, ch).guarded, 240);
  // without the typed confirmation, byos-cli keeps them
  const kept = await r.job('sync-apply', { device: 'usb', yes: true, decisions: decisionsFor(plan, ch), runId: plan.run_id });
  assert.equal(kept.ok, true);
  assert.ok(kept.events.some((e) => e.event === 'warn' && e.code === 'mass_delete_blocked'));
  assert.equal(kept.kinds.music.removed, 0);
});

test('mass delete confirmed by typing the count', async () => {
  const r = await start('mass2', 'sync_mass_delete');
  await r.link('music');
  await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const plan = (await r.call('GET', '/api/plan')).json;
  const ch = initialChoices(plan);
  setGroup(ch, plan.items.filter((i) => i.reason === 'deleted_on_computer' && !i.parent), 'delete');
  const gone = await r.job('sync-apply', { device: 'usb', yes: true, allowMassDelete: true, decisions: decisionsFor(plan, ch), runId: plan.run_id });
  assert.equal(gone.ok, true);
  assert.equal(gone.kinds.music.removed, 240);
});

test('locked by another computer, then taken over', async () => {
  const r = await start('locked', 'sync_locked');
  await r.link('music');
  await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const st = await r.job('sync-status', { deviceId: DEV });
  assert.equal(st.lock.held, true);
  const locked = await r.job('sync-apply', { device: 'usb', yes: true });
  assert.deepEqual(outcome(locked), { state: 'locked', by: locked.locked_by });
  assert.equal(locked.locked_by.name, 'ANA-LAPTOP');
  const taken = await r.job('sync-apply', { device: 'usb', yes: true, takeOver: true });
  assert.equal(outcome(taken).state, 'done');
});

test('paused when the HiBy goes away, then resumed without copying twice', async () => {
  const r = await start('pause', 'sync_pause');
  await r.link('music');
  const plan = await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const paused = await r.job('sync-apply', { device: 'usb', yes: true, then: 'rescan' });
  assert.deepEqual(outcome(paused), { state: 'paused', reason: 'link' });
  assert.equal(paused.resumable, true);
  const st = await r.job('sync-status', { deviceId: DEV });
  assert.equal(st.runs.length, 1);
  assert.equal(st.runs[0].resumable, true);
  const resumed = await r.job('sync-resume', { device: 'usb', then: 'rescan' });
  assert.equal(resumed.resumed, true);
  assert.equal(outcome(resumed).state, 'done');
  const total = plan.summary.music.add + plan.summary.music.update;
  assert.equal(paused.kinds.music.written + resumed.kinds.music.written, total); // nothing copied twice
  assert.equal((await r.job('sync-resume', { device: 'usb' })).status, 'nothing_to_resume');
});

test('the other ways a sync ends: USB storage, battery, failures, library not updated, no room', async () => {
  const cases = [
    ['sync_usb_pause', { state: 'paused', reason: 'usb_storage' }],
    ['sync_battery', { state: 'paused', reason: 'battery' }],
    ['sync_partial', { state: 'done_with_problems' }],
    ['sync_db_pending', { state: 'blocked_db' }],
  ];
  for (const [scenario, want] of cases) {
    const r = await start(scenario, scenario);
    await r.link('music');
    await r.job('sync-plan', { device: 'usb', kind: 'music' });
    const f = await r.job('sync-apply', { device: 'usb', yes: true, then: 'rescan' });
    assert.deepEqual(outcome(f), want, scenario);
    if (scenario === 'sync_partial') assert.equal(f.failures.length, 3);
  }
  const full = await start('full', 'card_full');
  await full.link('music');
  const p = await full.job('sync-plan', { device: 'usb', kind: 'music' });
  assert.equal(p.fits, false);
});

test('ask policy: files only on the HiBy wait for a decision', async () => {
  const r = await start('ask', 'ask_policy');
  await r.link('music');
  await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const plan = (await r.call('GET', '/api/plan')).json;
  const undecided = plan.items.filter((i) => i.needs_decision);
  assert.equal(undecided.length, 3);
  assert.ok(undecided.every((i) => i.default_choice === null));
  assert.deepEqual(Object.keys(decisionsFor(plan, initialChoices(plan))).sort(), undecided.map((i) => i.id).sort());
});

// ---- review fixes ---------------------------------------------------------------------------------

test('the idle check never replaces the previewed plan; choices for another plan are refused (finding 1)', async () => {
  const r = await start('race');
  await r.link('music');
  // the user opens the preview
  const p1 = await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const plan = (await r.call('GET', '/api/plan')).json;
  assert.equal(plan.run_id, p1.run_id);
  // the quiet idle check runs meanwhile: its own file, the previewed plan stays as it is
  const check = await r.job('sync-plan', { device: 'usb', kind: 'all', check: true });
  assert.equal(check.ok, true);
  assert.notEqual(check.run_id, plan.run_id);
  assert.equal((await r.call('GET', '/api/plan')).json.run_id, plan.run_id);
  assert.equal((await r.call('GET', '/api/plan?check=1')).json.run_id, check.run_id);
  const ch = initialChoices(plan);
  setGroup(ch, plan.items.filter((i) => i.reason === 'added_on_player' && !i.parent), 'delete');
  const dec = decisionsFor(plan, ch);
  assert.ok(Object.keys(dec).length > 0);
  // choices without the plan they were made on are refused
  const noRun = await r.job('sync-apply', { device: 'usb', yes: true, decisions: dec });
  assert.equal(noRun.status, 400);
  // another plan replaced the previewed one: the ids (i1, i2…) would name other files
  await r.job('sync-plan', { device: 'usb', kind: 'all' });
  const stale = await r.job('sync-apply', { device: 'usb', yes: true, decisions: dec, runId: plan.run_id });
  assert.equal(stale.ok, false);
  assert.equal(stale.status, 409);
  assert.equal(stale.code, 'plan_changed');
  assert.equal(outcome(stale).state, 'error');
  assert.equal(existsSync(path.join(r.app.cache, 'sync', 'decisions.json')), false); // nothing was handed to byos-cli
  assert.equal((await r.job('sync-apply', { device: 'usb', yes: true, decisions: dec, runId: 'bad id!' })).status, 400);
  // with the plan it was made on, it runs (on a copy byos-cli reads, not the shared plan file)
  const again = await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const plan2 = (await r.call('GET', '/api/plan')).json;
  assert.equal(plan2.run_id, again.run_id);
  const ch2 = initialChoices(plan2);
  setGroup(ch2, plan2.items.filter((i) => i.reason === 'added_on_player' && !i.parent), 'delete');
  const ok = await r.job('sync-apply', { device: 'usb', yes: true, then: 'rescan', decisions: decisionsFor(plan2, ch2), runId: plan2.run_id });
  assert.equal(ok.ok, true, JSON.stringify(ok).slice(0, 300));
  assert.equal(ok.kinds.music.removed, plan2.items.filter((i) => i.reason === 'added_on_player' && !i.parent).length);
  assert.equal(JSON.parse(readFileSync(path.join(r.app.cache, 'sync', 'plan-apply-v2.json'), 'utf8')).run_id, plan2.run_id);
});

test('Stop: the run ends cancelled, and once discarded it is not offered again (finding 2)', async () => {
  const r = await start('stop', 'ok', music, '40');
  await r.link('music');
  await r.job('sync-plan', { device: 'usb', kind: 'music' });
  const s = await r.call('POST', '/api/jobs', { command: 'sync-apply', params: { device: 'usb', yes: true, then: 'rescan' } });
  assert.equal(s.status, 202);
  const ev = await fetch(new URL(`/api/jobs/${s.json.id}/events?token=testtoken`, r.base));
  const reader = ev.body.getReader();
  let text = '';
  let cancelled = false;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    text += Buffer.from(value).toString('utf8');
    // Stop once the copies are under way
    if (!cancelled && text.includes('"op":"put"')) {
      cancelled = true;
      await r.call('POST', `/api/jobs/${s.json.id}/cancel`, {});
    }
  }
  const end = text.split('\n\n').find((b) => b.startsWith('event: end'));
  const final = JSON.parse(/^data: (.*)$/m.exec(end)[1]);
  assert.equal(final.code, 'cancelled');
  assert.equal(final.resumable, true); // byos-cli always says so…
  const o = outcome(final, { userPaused: false });
  assert.deepEqual(o, { state: 'cancelled', run: final.run_id }); // …but Stop is not Pause
  assert.deepEqual(outcome(final, { userPaused: true }), { state: 'paused', reason: 'user' });
  const st = await r.job('sync-status', { deviceId: DEV });
  assert.equal(st.runs.length, 1);
  assert.equal(pendingRun(st.runs).run_id, final.run_id); // offered until the page records the Stop
  assert.equal(pendingRun(st.runs, new Set([o.run])), null);
});

test('locked by another sync on this computer: no Take Over (finding 3)', async () => {
  const r = await start('lockhere', 'sync_locked_here');
  await r.link('music');
  await r.job('sync-plan', { device: 'usb', kind: 'music' });
  for (const takeOver of [false, true]) {
    const f = await r.job('sync-apply', { device: 'usb', yes: true, takeOver });
    const o = outcome(f);
    assert.equal(o.state, 'locked');
    assert.equal(o.by.same_computer, true);
    assert.equal(lockView(o.by).takeOver, false);
  }
});

// ---- translations -------------------------------------------------------------------------------------

test('the Sync tab and the computer library exist in English, French and Spanish', () => {
  const src = ['ui/sync.js', 'ui/media.js', 'lib/sync-flow.mjs'].map((f) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')).join('\n');
  const used = new Set([...src.matchAll(/\bt\(\s*'([A-Za-z_]+)'/g)].map((m) => m[1]));
  // the keys built from a value
  const kinds = ['music', 'audiobooks', 'ebooks', 'video'];
  const reasons = ['deleted_on_computer', 'excluded_on_computer', 'from_other_device', 'added_on_player', 'downloaded_on_player'];
  for (const k of kinds) for (const p of ['syKind_', 'sySync_', 'syChoose_', 'syGuard_', 'pvHeadAdd_']) used.add(p + k);
  used.add('pvHeadAdd_all');
  for (const v of ['keep', 'delete', 'ask']) { used.add(`syPolicy_${v}`); used.add(`syPolicyHint_${v}`); }
  for (const r of ['user', 'link', 'usb_storage', 'battery', 'interrupted']) { used.add(`syPaused_${r}`); used.add(`syPausedBody_${r}`); }
  for (const g of ['source_unavailable', 'volume_changed', 'scan_errors']) used.add(`pvGuardBody_${g}`);
  for (const s of ['add', 'update', 'move', 'player_only', 'conflict', 'blocked']) used.add(`pvSec_${s}`);
  for (const r of reasons) { used.add(`pvReason_${r}`); used.add(`pvReasonHint_${r}`); }
  for (const c of ['computer', 'player', 'both', 'skip']) { used.add(`pvConflict_${c}`); used.add(`pvConflictHint_${c}`); }
  for (const c of ['keep', 'delete', 'pull']) used.add(`pvChoice_${c}`);
  for (const w of ['edited_on_player', 'changed_both', 'different_content', 'other']) used.add(`pvConflictWhy_${w}`);
  for (const b of ['too_big_fat32', 'too_big', 'bad_path_too_long', 'bad_path_too_deep', 'bad_path', 'folder_full_fat', 'inside_other_link', 'other']) used.add(`pvBlocked_${b}`);
  for (const k of ['music', 'audiobooks', 'ebooks', 'video', 'all']) used.add(`syRetryFailed_${k}`);
  for (const k of ['pvReason_not_found', 'pvReasonHint_not_found_source_unavailable', 'pvReasonHint_not_found_volume_changed', 'syLocked', 'syLockedBody',
    'syLockedBodyNoAge', 'syLockedHere', 'syLockedHereBody', 'pvKeepAll', 'pvDeleteAll', 'pvCopyAll', 'err_plan_changed', 'err_E_IO']) used.add(k);
  for (const k of ['pcEmptyTitle', 'pcEmptyBody', 'pcNoLinksTitle', 'pcNoLinksBody', 'pcRebuild', 'pcScanningTags', 'pcPlaylistsNone', 'pcInfoReadOnly', 'pcTitle', 'tabSync', 'err_locked']) used.add(k);
  const missingEn = [...used].filter((k) => !(k in STRINGS.en));
  assert.deepEqual(missingEn, []);
  const keys = Object.keys(STRINGS.en).filter((k) => /^(sy|pv|pc)[A-Z]/.test(k) || k === 'tabSync' || k === 'libGoSync' || k === 'err_locked');
  assert.ok(keys.length > 200, String(keys.length));
  const ph = (s) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');
  for (const l of ['fr', 'es']) {
    assert.deepEqual(keys.filter((k) => !(k in STRINGS[l])), [], l);
    for (const k of keys) assert.equal(ph(STRINGS[l][k]), ph(STRINGS.en[k]), `${l}.${k}`);
  }
  assert.equal(existsSync(new URL('../ui/sync.js', import.meta.url)), true);
});

test('the page has no Music tab any more, and loads the Sync tab', () => {
  const html = readFileSync(new URL('../ui/index.html', import.meta.url), 'utf8');
  assert.match(html, /data-tab="sync"/);
  assert.doesNotMatch(html, /data-tab="music"/);
  assert.match(html, /id="sync-preview"/);
  assert.match(html, /id="computer-view"/);
  writeFileSync(path.join(tmp, 'ok'), 'x');
});
