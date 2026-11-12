// The Sync tab's rules (lib/sync-flow.mjs): migration, the preview's groups and choices, the
// DEC.json they become, the mass-delete confirmation, how a run ended, and its progress.

import test from 'node:test';
import assert from 'node:assert/strict';
import {
  applyLabel, applyProgress, currentAlbum, decisionsFor, deletionSummary, emptyProgress, fasterOverUsb, fits, folderName, freeAfter,
  groupChoice, groupPlan, guards, headline, initialChoices, kindChanges, linkPath, migrationPlan, needsTypedConfirm, nothingToDo,
  outcome, planEta, planIsEmpty, planTotals, playerOnlyCount, resultTotals, selectArgs, setChoice, setGroup, typedCountOk,
} from '../lib/sync-flow.mjs';
import { virtualWindow } from '../lib/media-format.mjs';
import { setLang, t } from '../ui/i18n.js';

const fmt = { count: (n) => String(n), bytes: (n) => `${Math.round(n / 1e6)} MB` };

function item(id, action, extra = {}) {
  return {
    id, kind: 'music', link: 'l1', action, reason: '', src_rel: null, dest: `Music/A/B/${id}.flac`, old_dest: null, size: 10e6,
    card_size: -1, title: id, default_choice: null, choices: [], ...extra,
  };
}
const po = (id, reason, extra = {}) => item(id, 'player_only', { reason, choices: ['delete', 'keep', 'pull'], default_choice: 'keep', card_size: 10e6, ...extra });

function samplePlan() {
  return {
    format: 2, kinds: ['music'], sd_free: 1e9, fits: true,
    summary: { music: { add: 2, eta_s: 120 } },
    links: [{ id: 'l1', kind: 'music', label: 'Music', guard: { tripped: false, reason: null } }],
    items: [
      item('a1', 'add'), item('a2', 'add'), item('a2l', 'add', { parent: 'a2', size: 2000 }),
      item('u1', 'update', { size: 30e6, card_size: 29e6 }),
      item('m1', 'move', { old_dest: 'Music/A/Old/m1.flac' }),
      item('r1', 'adopt'), item('r2', 'record'),
      po('d1', 'deleted_on_computer', { default_choice: 'delete' }),
      po('d2', 'deleted_on_computer', { default_choice: 'delete' }),
      po('d2l', 'deleted_on_computer', { parent: 'd2', default_choice: 'delete' }),
      po('p1', 'added_on_player'), po('p2', 'downloaded_on_player'),
      item('c1', 'conflict', { reason: 'edited_on_player', choices: ['computer', 'player', 'both', 'skip'], default_choice: 'skip' }),
      item('b1', 'blocked', { reason: 'too_big_fat32' }),
    ],
  };
}

test('migration: the Music tab folders and selection become music links', () => {
  assert.deepEqual(selectArgs({ all: true }), []);
  assert.deepEqual(selectArgs({ all: false, artists: ['abba'], albums: ['abba\u0000gold'], genres: ['jazz'] }),
    ['artist:abba', 'album:abba::gold', 'genre:jazz']);
  const sources = [
    { path: 'D:\\Music', exists: true, folder: 'tok1' },
    { path: 'E:\\More', exists: true, folder: 'tok2' },
    { path: 'C:\\drop', exists: true, temp: true, folder: 'tok3' }, // dropped files: not a link
    { path: 'F:\\Gone', exists: false, folder: null },
  ];
  const reqs = migrationPlan(sources, { all: false, artists: ['abba'], albums: [], genres: [] }, { layout: 'keep', remove: true, dest: 'Musica' }, []);
  assert.equal(reqs.length, 2);
  assert.deepEqual(reqs[0], { kind: 'music', folder: 'tok1', layout: 'keep', playerOnly: 'keep', select: ['artist:abba'], removeUnselected: true, cardRoot: 'Musica' });
  assert.equal(reqs[1].cardRoot, undefined); // byos-cli puts a second link under Music/<label>
  // nothing saved: the whole library, Artist/Album (the owner's default)
  assert.deepEqual(migrationPlan([sources[0]], null, null, []), [{ kind: 'music', folder: 'tok1', layout: 'artist-album', playerOnly: 'keep' }]);
  // a folder already linked is not linked again (case and trailing slash do not matter)
  const linked = [{ kind: 'music', local: { type: 'path', path: 'd:\\music\\' } }];
  assert.deepEqual(migrationPlan(sources, null, null, linked).map((r) => r.folder), ['tok2']);
  assert.equal(folderName('D:\\Music\\Rock\\'), 'Rock');
  assert.equal(linkPath({ local: { path: '/home/me/Music' } }), '/home/me/Music');
  assert.equal(linkPath({ local: 'C:\\x' }), 'C:\\x');
});

test('preview: sections, player-only groups, quiet and sidecar items', () => {
  const plan = samplePlan();
  const [g] = groupPlan(plan);
  assert.equal(g.kind, 'music');
  assert.deepEqual(g.add.map((i) => i.id), ['a1', 'a2']); // the .lrc follows its track
  assert.equal(g.children, 2);
  assert.equal(g.quiet, 2);
  assert.deepEqual(g.update.map((i) => i.id), ['u1']);
  assert.deepEqual(g.move.map((i) => i.id), ['m1']);
  assert.deepEqual(g.player_only.deleted_on_computer.map((i) => i.id), ['d1', 'd2']);
  assert.deepEqual(g.player_only.added_on_player.map((i) => i.id), ['p1']);
  assert.deepEqual(g.player_only.downloaded_on_player.map((i) => i.id), ['p2']);
  assert.equal(playerOnlyCount(g), 4);
  assert.deepEqual(g.conflict.map((i) => i.id), ['c1']);
  assert.deepEqual(g.blocked.map((i) => i.id), ['b1']);
  // Sync all: one block per kind, in the design's order
  const multi = groupPlan({ kinds: ['video', 'music'], items: [item('v', 'add', { kind: 'video' }), item('m', 'add')] });
  assert.deepEqual(multi.map((x) => x.kind), ['music', 'video']);
});

test('choices: per file, per group, and the DEC.json they become', () => {
  const plan = samplePlan();
  const ch = initialChoices(plan);
  assert.equal(ch.get('d1'), 'delete');
  assert.equal(ch.get('p1'), 'keep');
  assert.equal(ch.get('c1'), 'skip');
  assert.equal(ch.has('a1'), false); // copies take no choice
  assert.equal(ch.has('d2l'), false); // a sidecar follows its track
  assert.deepEqual(decisionsFor(plan, ch), {}); // nothing changed: byos-cli's own pre-ticks
  const g = groupPlan(plan)[0];
  assert.equal(groupChoice(ch, g.player_only.deleted_on_computer), 'delete');
  setChoice(ch, plan.items.find((i) => i.id === 'd1'), 'keep');
  assert.equal(groupChoice(ch, g.player_only.deleted_on_computer), 'mixed');
  assert.equal(setChoice(ch, plan.items.find((i) => i.id === 'c1'), 'delete'), false); // not one of its choices
  setChoice(ch, plan.items.find((i) => i.id === 'c1'), 'both');
  assert.equal(setGroup(ch, [...g.player_only.added_on_player, ...g.player_only.downloaded_on_player], 'delete'), 2);
  setChoice(ch, plan.items.find((i) => i.id === 'd2'), 'pull');
  assert.deepEqual(decisionsFor(plan, ch), { d1: 'keep', d2: 'pull', p1: 'delete', p2: 'delete', c1: 'both' });
  // an undecided item (policy "ask") is always written, so the run does what the preview showed
  const ask = { items: [po('q1', 'deleted_on_computer', { default_choice: null, needs_decision: true })] };
  const c2 = initialChoices(ask);
  assert.equal(c2.get('q1'), 'keep');
  assert.deepEqual(decisionsFor(ask, c2), { q1: 'keep' });
  assert.equal(planTotals(ask, c2).needsDecision, 1);
});

test('deletions: summary, DeleteGuard and the typed count', () => {
  const plan = samplePlan();
  const ch = initialChoices(plan);
  let del = deletionSummary(plan, ch);
  assert.deepEqual(del, { count: 2, bytes: 20e6, reasons: { deleted_on_computer: 2 }, guarded: 0, guardedBytes: 0 });
  assert.equal(needsTypedConfirm(del), false);
  // DeleteGuard: guarded items are never pre-ticked; ticking them asks for the typed count
  const guarded = { items: Array.from({ length: 240 }, (_, k) => po(`g${k}`, 'deleted_on_computer', { guard: true })), links: [{ id: 'l1', kind: 'music', guard: { tripped: true, reason: 'mass_delete' } }] };
  const gc = initialChoices(guarded);
  assert.equal(deletionSummary(guarded, gc).count, 0);
  setGroup(gc, guarded.items, 'delete');
  del = deletionSummary(guarded, gc);
  assert.equal(del.guarded, 240);
  assert.equal(needsTypedConfirm(del), true);
  assert.equal(typedCountOk('240', 240), true);
  assert.equal(typedCountOk(' 240 ', 240), true);
  assert.equal(typedCountOk('1,240', 1240), true);
  assert.equal(typedCountOk('1 240', 1240), true);
  assert.equal(typedCountOk('24', 240), false);
  assert.equal(typedCountOk('', 240), false);
  assert.deepEqual(guards(guarded), [{ link: 'l1', kind: 'music', label: undefined, reason: 'mass_delete' }]);
});

test('header line and Apply button in English, French and Spanish', () => {
  const plan = samplePlan();
  const ch = initialChoices(plan);
  setLang('en');
  assert.equal(headline(plan, ch, t, fmt), 'Add 2 songs (20 MB) · Update 1 · Move 1 · Remove 2 · 999 MB free after');
  assert.equal(applyLabel(plan, ch, t, fmt), 'Sync and Delete 2 Files from the HiBy');
  setGroup(ch, plan.items.filter((i) => i.action === 'player_only'), 'keep');
  assert.equal(applyLabel(plan, ch, t, fmt), 'Sync');
  setLang('fr');
  assert.equal(headline(plan, ch, t, fmt), 'Ajouter 2 morceaux (20 MB) · Mettre à jour 1 · Déplacer 1 · 979 MB libres ensuite');
  setLang('es');
  setChoice(ch, plan.items.find((i) => i.id === 'p1'), 'delete');
  assert.equal(applyLabel(plan, ch, t, fmt), 'Sincronizar y eliminar 1 archivos del HiBy');
  assert.equal(headline({ kinds: ['music', 'video'], items: [], sd_free: 5e9 }, new Map(), t, fmt), 'Nada que copiar · 5000 MB libres después');
  setLang('en');
});

test('space, nothing to do, and the plan estimate', () => {
  const plan = samplePlan();
  const ch = initialChoices(plan);
  const tot = planTotals(plan, ch);
  assert.equal(tot.bytesAdd, 20e6 + 1e6); // two adds and the update's growth
  assert.equal(tot.quiet, 2);
  assert.equal(freeAfter(plan, ch), 1e9 - 21e6 + 20e6);
  assert.equal(fits(plan, ch), true);
  assert.equal(fits({ ...plan, sd_free: 50e6 }, ch), false);
  assert.equal(planIsEmpty(plan), false);
  assert.equal(planIsEmpty({ items: [item('x', 'add', { parent: 'y' })] }), true);
  // the folder is unavailable: files listed (all kept), but nothing would change
  const kept = { items: [po('k1', 'deleted_on_computer', { guard: true })] };
  assert.equal(planIsEmpty(kept), false);
  assert.equal(nothingToDo(kept, initialChoices(kept)), true);
  const conflict = { items: [item('c', 'conflict', { choices: ['computer', 'skip'], default_choice: 'skip' })] };
  const cc = initialChoices(conflict);
  assert.equal(nothingToDo(conflict, cc), true);
  setChoice(cc, conflict.items[0], 'computer');
  assert.equal(nothingToDo(conflict, cc), false);
  assert.equal(planEta({ summary: { music: { eta_s: 100 }, video: { eta_s: 2000 } } }), 2100);
  assert.equal(fasterOverUsb({ transport: 'wifi' }, 1801), true);
  assert.equal(fasterOverUsb({ transport: 'wifi' }, 1800), false);
  assert.equal(fasterOverUsb({ transport: 'usb' }, 99999), false);
  assert.equal(kindChanges({ add: 3, update: 1, move: 2, player_only: { deleted_on_computer: 4, added_on_player: 9 } }), 10);
  assert.equal(kindChanges(null), null);
});

test('how a run ended: every Sync tab state', () => {
  assert.deepEqual(outcome({ ok: true, status: 'done', library: { action: 'rescan', ok: true } }), { state: 'done', restart: false });
  assert.deepEqual(outcome({ ok: true, status: 'done', library: { action: 'rebuild', ok: true, restart: true } }), { state: 'done', restart: true });
  assert.deepEqual(outcome({ ok: true, status: 'done', library: { action: 'rescan', ok: false, code: 'E_UNSUPPORTED' } }), { state: 'blocked_db' });
  assert.deepEqual(outcome({ ok: true, status: 'done_with_problems', failures: [{}] }), { state: 'done_with_problems' });
  assert.deepEqual(outcome({ ok: true, status: 'paused' }), { state: 'paused', reason: 'link' });
  assert.deepEqual(outcome({ ok: true, status: 'paused_usb_storage' }), { state: 'paused', reason: 'usb_storage' });
  assert.deepEqual(outcome({ ok: false, code: 'locked', locked_by: { name: 'ANA' } }), { state: 'locked', by: { name: 'ANA' } });
  assert.deepEqual(outcome({ ok: false, code: 'remote', btp: { name: 'E_BATTERY' } }), { state: 'paused', reason: 'battery' });
  assert.deepEqual(outcome({ ok: false, code: 'remote', btp: { name: 'E_SD_UNAVAILABLE' } }), { state: 'paused', reason: 'usb_storage' });
  assert.deepEqual(outcome({ ok: false, code: 'timeout' }), { state: 'paused', reason: 'link' });
  assert.deepEqual(outcome({ ok: false, code: 'cancelled', resumable: true }), { state: 'paused', reason: 'user' });
  assert.deepEqual(outcome({ ok: false, code: 'cancelled' }), { state: 'cancelled' });
  assert.deepEqual(outcome({ ok: false, code: 'cancelled' }, { userPaused: true }), { state: 'paused', reason: 'user' });
  assert.deepEqual(outcome({ ok: true, resumed: false, status: 'nothing_to_resume' }), { state: 'idle' });
  assert.deepEqual(outcome({ ok: false, code: 'usage' }), { state: 'error' });
  assert.deepEqual(outcome(null), { state: 'error' });
  const tot = resultTotals({ kinds: { music: { written: 3, bytes: 10, removed: 1 }, ebooks: { written: 2, bytes: 5, kept: 4 } } });
  assert.equal(tot.written, 5);
  assert.equal(tot.bytes, 15);
  assert.equal(tot.removed, 1);
  assert.equal(tot.kept, 4);
});

test('progress: overall, per file, phases, warnings', () => {
  const p = emptyProgress();
  applyProgress(p, { event: 'progress', op: 'scan', done: 4, total: 0 });
  assert.equal(p.phase, 'scan');
  applyProgress(p, { event: 'progress', op: 'move', path: 'Music/A/B/1.flac' });
  assert.equal(p.phase, 'move');
  applyProgress(p, { event: 'progress', op: 'sync', path: 'Music/ABBA/Gold/03 SOS.flac', done: 100, total: 1000, rate: 50, files_done: 3, files_total: 30 });
  assert.equal(p.phase, 'put');
  assert.deepEqual([p.done, p.total, p.rate, p.files_done, p.files_total], [100, 1000, 50, 3, 30]);
  applyProgress(p, { event: 'progress', op: 'put', path: 'Music/ABBA/Gold/03 SOS.flac', done: 5, total: 10 });
  assert.deepEqual([p.fileDone, p.fileTotal], [5, 10]);
  applyProgress(p, { event: 'warn', code: 'reconnecting' });
  assert.equal(p.reconnecting, true);
  applyProgress(p, { event: 'progress', op: 'put', path: 'x', done: 6, total: 10 });
  assert.equal(p.reconnecting, false);
  applyProgress(p, { event: 'warn', code: 'failed', path: 'x' });
  applyProgress(p, { event: 'warn', code: 'something_else' });
  assert.equal(p.warnings.length, 2);
  applyProgress(p, { event: 'progress', op: 'library' });
  assert.equal(p.phase, 'library');
  applyProgress(p, { event: 'progress', op: 'sync', done: 1000, total: 1000 });
  assert.equal(p.phase, 'library'); // the last overall line does not hide the library step
  assert.deepEqual(currentAlbum('Music/ABBA/Gold/03 SOS.flac'), { name: '03 SOS', album: 'Gold', artist: 'ABBA' });
  assert.equal(currentAlbum(null), null);
});

// The owner's library is over 30,000 tracks: every step of the preview stays fast at 50,000.
test('50,000 files: grouping, choices, decisions and the visible rows stay fast', () => {
  const items = [];
  for (let k = 0; k < 50000; k++) items.push(item(`i${k}`, 'add'));
  for (let k = 0; k < 2000; k++) items.push(po(`d${k}`, 'deleted_on_computer', { guard: true }));
  const plan = { kinds: ['music'], sd_free: 1e15, items };
  const t0 = performance.now();
  const [g] = groupPlan(plan);
  const ch = initialChoices(plan);
  setGroup(ch, g.player_only.deleted_on_computer, 'delete');
  const dec = decisionsFor(plan, ch);
  const del = deletionSummary(plan, ch);
  const tot = planTotals(plan, ch);
  const w = virtualWindow(30 * 25000, 330, 30, g.add.length, 6);
  const ms = performance.now() - t0;
  assert.equal(g.add.length, 50000);
  assert.equal(Object.keys(dec).length, 2000);
  assert.equal(JSON.stringify(dec).length < 64 * 1024, true); // well under the runner's request limit
  assert.equal(del.guarded, 2000);
  assert.equal(tot.add, 50000);
  assert.ok(w.end - w.start < 30, 'only the visible rows are drawn');
  assert.ok(ms < 1000, `took ${ms} ms`);
});
