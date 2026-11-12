// The device's Sync tab (docs/SYNC_LIBRARY_DESIGN.md section 10.2): one row per kind (music,
// audiobooks, books, video) with its linked folder on this computer and its options, Sync per
// kind and Sync All, the preview of the changes (copy, update, move, files only on the HiBy with
// Keep/Delete per file and per group, conflicts, files that cannot be copied), the typed
// confirmation for mass deletions, the progress with Pause and Resume, and the results. Every
// operation is a byos-cli job (link-*, sync-plan, sync-apply, sync-resume, sync-status,
// sync-unlock); the page names folders on this computer only by the runner's tokens. Names from
// the HiBy and from files only ever go into the page with textContent.

import { fold, fmtLeft, matches, secondsLeft, virtualWindow } from '/media-format.js';
import {
  KINDS, REASONS, applyLabel, applyProgress, currentAlbum, decisionsFor, deletionSummary, emptyProgress, fasterOverUsb,
  fits, folderName, groupChoice, groupPlan, groupView, guards, headline, initialChoices, kindChanges, linkPath, linksOf, lockView, migrationPlan,
  needsTypedConfirm, nothingToDo, outcome, pendingRun, planEta, planIsEmpty, planTotals, playerOnlyCount, resultTotals, selectionEmpty,
  selectionRequest, setChoice, setGroup, typedCountOk,
} from '/sync-flow.js';

let C; // the app's helpers (initSync)
let M; // the media module's shared helpers (icon, ICONS, toast, messageCard)
const ROW_H = 30;

const sy = {
  deviceId: null, links: null, linksError: null, loading: false, catalog: null, status: null,
  summary: {}, guards: [], checking: false, checkedAt: 0,
  state: 'idle', kinds: null, plan: null, choices: new Map(), open: new Set(), progress: emptyProgress(), final: null,
  job: null, userPaused: false, pause: null, lockedBy: null, error: null, lastApply: null, expanded: new Set(), filter: '',
  pendingSel: new Map(), selTimer: null, selLinks: new Map(), linkJobs: new Set(), recheck: null, demo: null,
};

const $ = (id) => document.getElementById(id);
const t = (...a) => C.t(...a);
const el = (...a) => C.el(...a);
const lang = () => C.getLang();
const fmtBytes = (n) => C.fmtBytes(n);
const fmtCount = (n) => new Intl.NumberFormat(lang()).format(n ?? 0);
const fmt = { count: fmtCount, bytes: (n) => fmtBytes(n) };
// A text with a count: the number formatted for the language, and the singular form when it applies.
const tc = (key, n, vars = {}) => t(key, { n: fmtCount(n), count: Number(n) || 0, ...vars });
const device = () => C.selectedDevice();
const spec = () => C.deviceSpec(device());
const reachable = () => !!device() && device().reachable !== false;
// the connection as the "Faster over USB" hint sees it (the demo can show it on a USB HiBy)
const link = () => (sy.demoWifi ? { ...device(), transport: 'wifi' } : device());
const busy = () => ['scanning', 'planning', 'syncing', 'building'].includes(sy.state);

function storage(key) {
  try { return JSON.parse(localStorage.getItem(key) ?? 'null'); } catch { return null; }
}

// Runs the user stopped or discarded, per HiBy: not offered as "An earlier sync didn't finish"
// again. byos-cli keeps their journals, so what they copied is still known to the next sync.
const DISCARDED = 'byos.sync.discarded';
const discardedHere = new Set(); // also this session's, when the browser keeps no storage
function discardedRuns() {
  const all = storage(DISCARDED);
  return new Set([...(Array.isArray(all?.[sy.deviceId]) ? all[sy.deviceId] : []), ...discardedHere]);
}
function discardRun(runId) {
  if (!runId || !sy.deviceId) return;
  discardedHere.add(runId);
  try {
    const all = storage(DISCARDED) ?? {};
    const list = (Array.isArray(all[sy.deviceId]) ? all[sy.deviceId] : []).filter((x) => x !== runId);
    list.push(runId);
    all[sy.deviceId] = list.slice(-50);
    localStorage.setItem(DISCARDED, JSON.stringify(all));
  } catch { /* the run may be offered again; resuming it is still safe */ }
}

// A music link whose chooser has nothing ticked cannot sync: byos-cli would still use the link's
// last selection, which the checkboxes no longer show.
function selectionBlocked(l) {
  return l.kind === 'music' && l.enabled && selectionEmpty(sy.pendingSel.get(l.id));
}
function syncBlocked(kind) {
  return (sy.links ?? []).some((l) => (kind === 'all' || l.kind === kind) && selectionBlocked(l));
}

// ---- loading the links -------------------------------------------------------------------------

async function loadLinks({ force = false } = {}) {
  const d = device();
  if (!d) return;
  if (sy.deviceId !== d.id) {
    Object.assign(sy, { deviceId: d.id, links: null, summary: {}, guards: [], status: null, checkedAt: 0, plan: null, state: busy() ? sy.state : 'idle' });
  }
  if (sy.loading || (sy.links && !force)) { render(); return; }
  sy.loading = true;
  render();
  const links = [];
  const final = await C.runCli('link-ls', { deviceId: d.id }, { quiet: true, onEvent: (ev) => { if (ev.event === 'entry') links.push(ev); } });
  sy.loading = false;
  if (sy.deviceId !== d.id) return;
  if (!final.ok) { sy.linksError = final; sy.links = []; render(); return; }
  sy.linksError = null;
  sy.links = final.links ?? links;
  if (await migrate(d.id)) return; // migrate() loads the links again
  render();
  loadCatalog();
  if (sy.demo) { demoSyncStep(); return; }
  loadStatus();
  if (reachable() && Date.now() - sy.checkedAt > 60000) checkChanges();
}

// The Music tab's folders and selection become music links, once per HiBy (design 2.1).
async function migrate(id) {
  let m;
  try { m = await C.api(`/api/migration?device=${id}`); } catch { return false; }
  if (m.migrated) return false;
  const reqs = migrationPlan(m.sources, storage('byos.music.sel'), storage('byos.music.opts'), sy.links);
  let made = 0;
  for (const r of reqs) {
    const final = await C.runCli('link-add', { deviceId: id, ...r }, { title: t('syMigrating') });
    if (final.ok) made++;
  }
  try { await C.api('/api/migration/done', { method: 'POST', body: { device: id } }); } catch { /* next time */ }
  if (!made) return false;
  M.toast(t('syMigrated', { n: made }));
  sy.links = null;
  await loadLinks({ force: true });
  return true;
}

async function loadCatalog({ read = false } = {}) {
  if (!linksOf(sy.links, 'music').length) return;
  try { sy.catalog = await C.api('/api/catalog'); render(); return; } catch { /* the computer's library is not read yet */ }
  // the chooser needs the music's names: read the folders once (the computer's library shares it)
  if (!read || sy.catalogReading) return;
  sy.catalogReading = true;
  const final = await C.runCli('music-scan', {}, { title: t('syReadingLibrary'), quiet: true });
  sy.catalogReading = false;
  if (final.ok) await loadCatalog();
}

async function loadStatus() {
  const d = device();
  if (!d) return;
  const final = await C.runCli('sync-status', { deviceId: d.id }, { quiet: true });
  if (sy.deviceId !== d.id || !final.ok) return;
  sy.status = final;
  // an unfinished run: offered as paused, so it can be resumed (not one the user stopped or discarded)
  const run = pendingRun(final.runs, discardedRuns());
  if (run && sy.state === 'idle') {
    sy.state = 'paused';
    sy.pause = { reason: run.status === 'paused_usb_storage' ? 'usb_storage' : run.status === 'paused' ? 'link' : 'interrupted', run };
  }
  render();
}

// The idle line of each kind: a quiet plan of everything, nothing changes on the HiBy. It writes
// its own plan file (check: true), and never runs while a plan is open, paused or locked: the
// preview's choices name items of the plan that sync-apply reads.
function checkAllowed() {
  return !sy.checking && !busy() && !sy.plan && !['preview', 'paused', 'locked'].includes(sy.state)
    && reachable() && (sy.links ?? []).some((l) => l.enabled);
}

async function checkChanges() {
  if (!checkAllowed()) return;
  sy.checking = true;
  render();
  const final = await C.runCli('sync-plan', { device: spec(), kind: 'all', check: true }, { quiet: true });
  sy.checking = false;
  sy.checkedAt = Date.now();
  if (final.ok) {
    sy.summary = final.summary ?? {};
    try { sy.guards = guards(await C.api('/api/plan?check=1')); } catch { sy.guards = []; }
  }
  render();
}

function scheduleCheck(ms = 1200) {
  clearTimeout(sy.recheck);
  sy.recheck = setTimeout(() => { sy.recheck = null; checkChanges(); }, ms);
}

// ---- link changes --------------------------------------------------------------------------------

async function linkJob(command, params, title) {
  const final = await C.runCli(command, { deviceId: sy.deviceId, ...params }, { title, quiet: !title });
  if (!final.ok) M.toast(C.errorMessage(final), 'fail');
  await loadLinks({ force: true });
  return final;
}

async function chooseFolder(kind, link) {
  let r;
  try { r = await C.api('/api/links/pick', { method: 'POST', body: { kind } }); } catch (e) { M.toast(e.message, 'fail'); return; }
  if (!r.folder) return;
  if (link) {
    const final = await linkJob('link-set', { id: link.id, folder: r.folder }, t('syLinking', { name: r.name }));
    if (final.ok) sy.summary = {};
  } else {
    const params = { kind, folder: r.folder };
    if (kind === 'music') params.layout = 'artist-album';
    await linkJob('link-add', params, t('syLinking', { name: r.name }));
  }
  sy.checkedAt = 0;
  if (kind === 'music') sy.catalog = null;
  checkChanges();
}

async function unlink(link) {
  const r = await C.sheet({ title: t('syUnlinkTitle', { name: link.label || folderName(linkPath(link)) }), body: t('syUnlinkBody'), okText: t('syUnlink'), danger: true });
  if (!r.ok) return;
  await linkJob('link-rm', { id: link.id }, t('syUnlink'));
  sy.summary = {};
  sy.checkedAt = 0;
  checkChanges();
}

// One option of a link: shown at once, saved by link-set, then the kind's changes are counted again.
// Returns a promise of the saved link; startSync waits for the ones still on their way.
function setLink(link, params) {
  Object.assign(link, params.__local ?? {});
  delete params.__local;
  const job = C.runCli('link-set', { deviceId: sy.deviceId, id: link.id, ...params }, { quiet: true }).then((final) => {
    sy.linkJobs.delete(job);
    if (!final.ok) { M.toast(C.errorMessage(final), 'fail'); loadLinks({ force: true }); return final; }
    if (final.link) Object.assign(link, final.link);
    if (params.select || params.selectAll) sy.pendingSel.delete(link.id);
    sy.checkedAt = 0;
    scheduleCheck();
    return final;
  });
  sy.linkJobs.add(job);
  return job;
}

// The chooser: names are sent to byos-cli after a short pause (one link-set per burst of clicks).
function selectionChanged(link) {
  clearTimeout(sy.selTimer);
  sy.selLinks.set(link.id, link);
  sy.selTimer = setTimeout(() => flushSelection(), 700);
}

// Sends the chooser's pending selections now (also before a sync, so the plan uses what the
// checkboxes show). An empty selection is not sent: that link cannot sync until something is
// chosen (selectionBlocked).
function flushSelection() {
  clearTimeout(sy.selTimer);
  sy.selTimer = null;
  const jobs = [];
  for (const link of sy.selLinks.values()) {
    const s = sy.pendingSel.get(link.id);
    const req = s ? selectionRequest(s) : null;
    if (req) jobs.push(setLink(link, req));
  }
  sy.selLinks.clear();
  return Promise.all(jobs);
}

// ---- sync: plan, preview, apply ------------------------------------------------------------------

async function startSync(kind) {
  if (busy() || !reachable()) return;
  if (syncBlocked(kind)) { M.toast(t('syChooseSome'), 'fail'); return; }
  sy.kinds = kind;
  sy.state = 'planning';
  sy.progress = emptyProgress();
  sy.error = null;
  sy.plan = null;
  render();
  // the options and the chooser's clicks reach byos-cli before the plan is made
  clearTimeout(sy.recheck);
  await flushSelection();
  await Promise.all([...sy.linkJobs]);
  clearTimeout(sy.recheck);
  const warns = [];
  const final = await C.runCli('sync-plan', { device: spec(), kind }, {
    title: t('syJobPlan'),
    onEvent: (ev) => {
      if (ev.event === 'warn') warns.push(ev);
      if (ev.event === 'progress') { applyProgress(sy.progress, ev); renderActive(); }
    },
  });
  if (!final.ok) { sy.state = 'error'; sy.error = final; render(); return; }
  let plan;
  try { plan = await C.api('/api/plan'); } catch (e) { sy.state = 'error'; sy.error = { code: 'internal', message: e.message }; render(); return; }
  if (kind === 'all') { sy.summary = final.summary ?? {}; sy.checkedAt = Date.now(); sy.guards = guards(plan); }
  else { sy.summary = { ...sy.summary, ...(final.summary ?? {}) }; sy.guards = [...sy.guards.filter((g) => g.kind !== kind), ...guards(plan)]; }
  sy.plan = plan;
  sy.choices = initialChoices(plan);
  sy.open = new Set(['player_only', 'conflict']);
  sy.state = 'preview';
  render();
  openPreview();
}

async function apply() {
  const plan = sy.plan;
  if (!plan) return;
  const del = deletionSummary(plan, sy.choices);
  let allowMassDelete = false;
  if (needsTypedConfirm(del)) {
    const r = await C.sheet({
      title: tc('pvMassTitle', del.count),
      body: t('pvMassBody', { n: del.count, count: del.count, size: fmtBytes(del.bytes) }),
      okText: tc('pvDeleteN', del.count), danger: true, input: '',
      validate: (v) => (typedCountOk(v, del.count) ? null : t('pvTypeWrong', { n: del.count })),
    });
    if (!r.ok) return;
    allowMassDelete = true;
  }
  closePreview();
  // runId: the runner refuses the choices if the plan file is no longer this plan
  const params = { device: spec(), then: 'rescan', yes: true, decisions: decisionsFor(plan, sy.choices), runId: plan.run_id };
  if (allowMassDelete) params.allowMassDelete = true;
  sy.lastApply = params;
  await run('sync-apply', params);
}

async function run(command, params) {
  sy.lastRun = { command, params };
  sy.state = 'syncing';
  sy.userPaused = false;
  sy.final = null;
  sy.pause = null;
  if (command === 'sync-apply' || !sy.progress.files_total) sy.progress = emptyProgress();
  const tot = sy.plan ? planTotals(sy.plan, sy.choices) : null;
  if (tot && command === 'sync-apply') { sy.progress.files_total = tot.add + tot.update; sy.progress.total = tot.bytesAdd; }
  render();
  const final = await C.runCli(command, params, {
    title: t(command === 'sync-resume' ? 'syJobResume' : 'syJobApply'),
    onStarted: (job) => { sy.job = job; },
    onEvent: (ev) => {
      applyProgress(sy.progress, ev);
      if (ev.event === 'progress' && sy.progress.phase === 'library' && sy.state === 'syncing') { sy.state = 'building'; render(); return; }
      renderActive();
    },
  });
  sy.job = null;
  // sync-resume with nothing left (the run never started, as with a low battery): apply again
  if (command === 'sync-resume' && final.ok && final.resumed === false && sy.lastApply) return run('sync-apply', sy.lastApply);
  sy.final = final;
  const o = outcome(final, { userPaused: sy.userPaused });
  sy.state = o.state;
  if (o.state === 'paused') sy.pause = { reason: o.reason, runId: final.run_id ?? null };
  if (o.state === 'locked') sy.lockedBy = o.by;
  if (o.state === 'error') sy.error = final;
  if (o.state === 'cancelled') {
    // Stop: the run is not offered again (its journal stays, so the next sync goes on from it)
    discardRun(o.run);
    sy.state = 'idle';
    sy.plan = null;
    sy.final = null;
    M.toast(t('syStopped'));
  }
  if (['done', 'done_with_problems', 'blocked_db'].includes(o.state)) { sy.plan = null; sy.summary = {}; sy.checkedAt = 0; C.libraryChanged?.(); }
  render();
  if (o.state !== 'paused') loadStatus();
}

function pause() {
  if (!sy.job?.id) return;
  sy.userPaused = true;
  C.api(`/api/jobs/${sy.job.id}/cancel`, { method: 'POST', body: {} }).catch(() => {});
}

async function stop() {
  if (!sy.job?.id) return;
  const r = await C.sheet({ title: t('syStopTitle'), body: t('syStopBody'), okText: t('syStop'), danger: true });
  if (!r.ok || !sy.job?.id) return;
  sy.userPaused = false;
  C.api(`/api/jobs/${sy.job.id}/cancel`, { method: 'POST', body: {} }).catch(() => {});
}

function resume(takeOver = false) {
  const params = { device: spec(), then: 'rescan' };
  if (takeOver) params.takeOver = true;
  return run('sync-resume', params);
}

async function takeOver() {
  const name = sy.lockedBy?.name ?? '?';
  const r = await C.sheet({ title: t('syTakeOverTitle', { name }), body: t('syTakeOverBody', { name }), okText: t('syTakeOver') });
  if (!r.ok) return;
  if (sy.lastApply && !sy.pause) await run('sync-apply', { ...sy.lastApply, takeOver: true });
  else await resume(true);
}

// The same run again after "another sync is running on this computer".
function tryAgain() {
  const last = sy.lastRun;
  return last ? run(last.command, last.params) : resume();
}

async function discard() {
  const id = sy.pause?.run?.run_id ?? sy.pause?.runId;
  const r = await C.sheet({ title: t('syDiscardTitle'), body: t('syDiscardBody'), okText: t('syDiscard'), danger: true });
  if (!r.ok) return;
  discardRun(id);
  backToIdle();
}

function backToIdle() {
  sy.state = 'idle';
  sy.final = null;
  sy.error = null;
  sy.pause = null;
  sy.plan = null;
  render();
  checkChanges();
}

// ---- rendering: the tab ----------------------------------------------------------------------------

const KIND_ICON = { music: 'note', audiobooks: 'headphones', ebooks: 'book', video: 'film' };

// The app re-renders on every device refresh (every 5 s): the tab is only rebuilt when something
// it shows changed, so open options, a half-typed filter and animations stay as they are.
let syncKey = '';
export function renderSync() {
  if (!C || !$('sync-body')) return;
  const d = device();
  const key = JSON.stringify([lang(), d?.id, d?.reachable, C.appState.tab, C.appState.status?.sd?.sd_free ?? null]);
  if (key === syncKey) return;
  syncKey = key;
  render();
}

function render() {
  const body = $('sync-body');
  if (!body || !C) return;
  // the chooser's lists and filter keep their scroll and focus across a redraw
  const lists = [...body.querySelectorAll('.pick-list')].map((n) => n.scrollTop);
  const typing = document.activeElement?.classList.contains('filter') ? document.activeElement.selectionStart : null;
  draw(body);
  body.querySelectorAll('.pick-list').forEach((n, i) => { n.scrollTop = lists[i] ?? 0; });
  if (typing !== null) {
    const f = body.querySelector('.sy-chooser .filter');
    if (f) { f.focus(); f.setSelectionRange(typing, typing); }
  }
}

function draw(body) {
  body.replaceChildren();
  const d = device();
  if (!d) return;
  if (sy.links === null) { body.append(el('div', { class: 'card lib-loading' }, el('span', { class: 'spinner' }))); return; }
  body.append(heroCard());
  const active = activeCard();
  if (active) body.append(active);
  for (const k of KINDS) body.append(kindCard(k));
  body.append(el('p', { class: 'muted small-text sy-foot', text: t('syFoot') }));
}

function heroCard() {
  const d = device();
  const sd = C.appState.status?.sd;
  const enabled = (sy.links ?? []).filter((l) => l.enabled);
  const last = Math.max(0, ...(sy.status?.links ?? []).map((l) => l.last_sync ?? 0));
  const sub = [];
  if (last) sub.push(t('syLastSync', { age: M.ageText(last) }));
  else if (enabled.length) sub.push(t('syNeverSynced'));
  if (sd?.sd_total) sub.push(t('syCardFree', { free: fmtBytes(sd.sd_free), total: fmtBytes(sd.sd_total) }));
  if (sy.checking) sub.push(t('syChecking'));
  const all = el('button', {
    class: 'btn primary sy-all', text: t('sySyncAll'),
    disabled: busy() || !reachable() || !enabled.length || sy.state === 'preview' || syncBlocked('all'),
    title: syncBlocked('all') ? t('syChooseSome') : null,
    onclick: () => startSync('all'),
  });
  const cap = el('div', { class: 'capacity sy-cap' });
  if (sd?.sd_total) {
    const used = sd.sd_total - sd.sd_free;
    const span = el('span', { class: 'used' });
    cap.append(span);
    requestAnimationFrame(() => { span.style.flexBasis = `${(100 * used) / sd.sd_total}%`; });
  }
  return el('div', { class: 'card sy-hero' },
    el('div', { class: 'sy-hero-row' },
      el('div', { class: 'sy-hero-icon' }, M.icon(M.ICONS.sync)),
      el('div', { class: 'grow' },
        el('h2', { text: t('syTitle', { name: d.name }) }),
        el('p', { class: 'muted', text: sub.join(' · ') || t('syIntro') })),
      all),
    sd?.sd_total ? cap : null,
    reachable() ? null : el('p', { class: 'sy-note warn', text: t('syOffline') }));
}

function kindTitle(k) { return t(`syKind_${k}`); }

// The line under a kind's name: what a sync would do, or why it cannot.
function kindStatus(k, links) {
  if (!links.length) return { text: t('syNotLinked'), cls: '' };
  if (!links.some((l) => l.enabled)) return { text: t('syOff'), cls: '' };
  if (syncBlocked(k)) return { text: t('syChooseSomeShort'), cls: 'danger' };
  const g = sy.guards.find((x) => x.kind === k);
  if (g && g.reason !== 'mass_delete') return { text: t(`syGuard_${k}`), cls: 'danger' };
  if (g) return { text: t('syGuardMass'), cls: 'danger' };
  const s = sy.summary[k];
  if (!s) return { text: sy.checking ? t('syChecking') : reachable() ? '' : t('syOfflineShort'), cls: '' };
  const n = kindChanges(s);
  if (!n) return { text: t('syUpToDate'), cls: 'ok' };
  return { text: tc('syChanges', n), cls: 'accent' };
}

function kindCard(k) {
  const links = linksOf(sy.links, k);
  const st = kindStatus(k, links);
  const enabled = links.some((l) => l.enabled);
  const head = el('div', { class: 'sy-kind-head' },
    el('div', { class: `sy-kind-icon k-${k}` }, M.icon(M.ICONS[KIND_ICON[k]])),
    el('div', { class: 'grow' },
      el('h3', { text: kindTitle(k) }),
      el('p', { class: `sy-status ${st.cls}`, text: st.text })),
    links.length ? toggle(enabled, (on) => { for (const l of links) setLink(l, { enabled: on, __local: { enabled: on } }); render(); }, t('syEnable', { kind: kindTitle(k) })) : null,
    el('button', {
      class: 'btn', text: t(`sySync_${k}`), disabled: busy() || !reachable() || !enabled || sy.state === 'preview' || syncBlocked(k),
      onclick: () => startSync(k),
    }));
  const card = el('div', { class: `card sy-kind${enabled ? '' : ' off'}`, 'data-kind': k }, head);
  if (!links.length) {
    card.append(el('div', { class: 'sy-empty' },
      el('p', { class: 'muted', text: t(`syChoose_${k}`) }),
      el('button', { class: 'btn', text: t('syChooseFolder'), disabled: busy(), onclick: () => chooseFolder(k, null) })));
  }
  for (const l of links) card.append(linkRow(k, l));
  if (k === 'video') card.append(el('p', { class: 'sy-note', text: t('syVideoNote') }));
  return card;
}

function toggle(on, change, label) {
  const input = el('input', { type: 'checkbox', role: 'switch', 'aria-label': label, checked: on, disabled: busy() });
  input.addEventListener('change', () => change(input.checked));
  return el('label', { class: 'switch', title: label }, input, el('span', { class: 'switch-track' }));
}

function linkRow(k, l) {
  const p = linkPath(l);
  const g = sy.guards.find((x) => x.link === l.id);
  const open = sy.expanded.has(l.id);
  const row = el('div', { class: `sy-link${g && g.reason !== 'mass_delete' ? ' missing' : ''}` },
    el('div', { class: 'sy-folder', title: p },
      M.icon(M.ICONS.folder, 'i sy-folder-i'),
      el('div', { class: 'sy-folder-t' }, el('span', { class: 'sy-folder-name', text: l.label || folderName(p) }), el('span', { class: 'sy-folder-path', text: p }))),
    el('div', { class: 'sy-arrow', 'aria-hidden': 'true', text: '→' }),
    el('div', { class: 'sy-dest' }, M.icon(M.ICONS.sd, 'i sy-folder-i'), el('span', { text: t('syOnHiby', { root: `${l.card_root}/` }) })),
    el('span', { class: 'spacer' }),
    el('button', { class: 'btn link', text: t('syChange'), disabled: busy(), onclick: () => chooseFolder(k, l) }),
    el('button', {
      class: `btn disclose${open ? ' open' : ''}`, 'aria-expanded': String(open), text: t('syOptions'),
      onclick: () => { if (open) sy.expanded.delete(l.id); else sy.expanded.add(l.id); render(); },
    }));
  const wrap = el('div', { class: 'sy-link-wrap' }, row);
  if (g && g.reason !== 'mass_delete') {
    wrap.append(el('div', { class: 'sy-guard' }, M.icon(M.ICONS.warn, 'i'), el('span', { class: 'grow', text: t(`syGuard_${k}`) }),
      el('button', { class: 'btn', text: t('syChooseAgain'), onclick: () => chooseFolder(k, l) })));
  }
  if (open) wrap.append(optionsPanel(k, l));
  return wrap;
}

function optionsPanel(k, l) {
  const panel = el('div', { class: 'sy-options' });
  const left = el('div', { class: 'sy-opt-col' });
  const right = el('div', { class: 'sy-opt-col' });
  if (k === 'music') {
    const chosen = sy.pendingSel.get(l.id)?.all === false || l.selection?.mode === 'chosen';
    const radio = (all, label) => el('label', { class: 'radio' },
      el('input', {
        type: 'radio', name: `sel-${l.id}`, checked: all ? !chosen : chosen, disabled: busy(),
        onchange: () => {
          const s = currentSel(l);
          s.all = all;
          sy.pendingSel.set(l.id, s);
          selectionChanged(l);
          render();
        },
      }), el('span', { text: label }));
    left.append(el('div', { class: 'radios' }, radio(true, t('syncEntire')), radio(false, t('syncSelected'))));
    const layout = el('select', { disabled: busy() },
      el('option', { value: 'artist-album', text: t('layoutArtistAlbum'), selected: l.layout === 'artist-album' }),
      el('option', { value: 'keep', text: t('layoutKeep'), selected: l.layout === 'keep' }));
    layout.addEventListener('change', () => setLink(l, { layout: layout.value, __local: { layout: layout.value } }));
    right.append(el('label', { class: 'field' }, el('span', { text: t('optLayout') }), layout));
  }
  const pol = el('div', { class: 'segmented small sy-policy', role: 'radiogroup', 'aria-label': t('syPlayerOnly') });
  for (const v of ['keep', 'delete', 'ask']) {
    pol.append(el('button', {
      role: 'radio', 'aria-selected': String((l.player_only ?? 'keep') === v), 'aria-checked': String((l.player_only ?? 'keep') === v), text: t(`syPolicy_${v}`), disabled: busy(),
      onclick: () => { setLink(l, { playerOnly: v, __local: { player_only: v } }); render(); },
    }));
  }
  right.append(el('div', { class: 'field' }, el('span', { text: t('syPlayerOnly') }), pol, el('span', { class: 'opt-hint', text: t(`syPolicyHint_${l.player_only ?? 'keep'}`) })));
  if (k === 'music') {
    // only means something with chosen artists, albums and genres; turning it on asks first (design 3.3.2)
    const chosen = sy.pendingSel.get(l.id)?.all === false || (sy.pendingSel.get(l.id)?.all !== true && l.selection?.mode === 'chosen');
    const cb = el('input', { type: 'checkbox', checked: !!l.remove_unselected, disabled: busy() || !chosen });
    cb.addEventListener('change', async () => {
      if (cb.checked) {
        const r = await C.sheet({ title: t('syRemoveTitle'), body: t('syRemoveBody'), okText: t('syRemoveOk'), danger: true });
        if (!r.ok) { cb.checked = false; return; }
      }
      setLink(l, { removeUnselected: cb.checked, __local: { remove_unselected: cb.checked } });
    });
    left.append(el('label', { class: `opt${chosen ? '' : ' off'}` }, cb, el('span', {}, el('span', { text: t('optRemove') }),
      el('span', { class: 'opt-hint', text: chosen ? t('optRemoveHint') : t('syRemoveNeedsChosen') }))));
  }
  left.append(el('button', { class: 'btn link danger sy-unlink', text: t('syUnlinkFolder'), disabled: busy(), onclick: () => unlink(l) }));
  panel.append(el('div', { class: 'sy-opt-grid' }, left, right));
  if (k === 'music' && (sy.pendingSel.get(l.id)?.all === false || l.selection?.mode === 'chosen')) panel.append(chooser(l));
  return panel;
}

function currentSel(l) {
  if (sy.pendingSel.has(l.id)) return sy.pendingSel.get(l.id);
  const s = l.selection ?? {};
  return {
    all: s.mode !== 'chosen',
    artists: new Set(s.artists ?? []),
    albums: new Set((s.albums ?? []).map((a) => `${a.artist}\u0000${a.title}`)),
    genres: new Set(s.genres ?? []),
  };
}

// The branch's chooser (iTunes' "Selected artists, albums and genres") on the computer's library.
function chooser(l) {
  const cat = sy.catalog;
  if (!cat?.tracks) {
    loadCatalog({ read: true });
    return el('div', { class: 'sy-chooser-wait muted small-text' }, el('span', { class: 'spinner' }), ` ${t('syReadingLibrary')}`);
  }
  const s = currentSel(l);
  s.all = false;
  sy.pendingSel.set(l.id, s);
  const has = (set, name) => { const f = fold(name); for (const x of set) if (fold(x) === f) return true; return false; };
  const toggleIn = (set, name, on) => {
    for (const x of [...set]) if (fold(x) === fold(name)) set.delete(x);
    if (on) set.add(name);
  };
  const filter = el('input', { type: 'search', class: 'filter', placeholder: t('syncFilter'), value: sy.filter });
  const boxes = el('div', { class: 'pick-boxes' });
  const box = (title, items, set) => {
    const shown = items.filter((i) => matches(sy.filter, i.label, i.sub ?? ''));
    const count = () => items.filter((i) => has(set, i.key)).length;
    const head = el('span', { class: 'pick-count', text: count() ? tc('selCount', count()) : '' });
    const list = el('ul', { class: 'pick-list' });
    for (const it of shown.slice(0, 2000)) {
      const cb = el('input', { type: 'checkbox', checked: has(set, it.key) });
      cb.addEventListener('change', () => {
        const wasEmpty = selectionEmpty(s);
        toggleIn(set, it.key, cb.checked);
        head.textContent = count() ? tc('selCount', count()) : '';
        selectionChanged(l);
        // nothing chosen any more, or something again: the Sync buttons and the message follow
        if (selectionEmpty(s) !== wasEmpty) render();
      });
      list.append(el('li', {}, el('label', { class: 'pick' }, cb,
        el('span', { class: 'pick-t' }, el('span', { text: it.label }), it.sub ? el('span', { class: 'pick-sub', text: it.sub }) : null),
        el('span', { class: 'pick-n', text: String(it.count) }))));
    }
    return el('div', { class: 'pick-box' }, el('div', { class: 'pick-head' }, el('span', { text: title }), head), list);
  };
  const draw = () => boxes.replaceChildren(
    box(t('artists'), cat.artists.map((a) => ({ key: a.name, label: a.name, count: a.tracks })), s.artists),
    box(t('albums'), cat.albums.map((a) => ({ key: `${a.artist}\u0000${a.title}`, label: a.title, sub: a.artist, count: a.tracks.length })), s.albums),
    box(t('genres'), cat.genres.map((g) => ({ key: g.name, label: g.name, count: g.tracks })), s.genres));
  filter.addEventListener('input', () => { sy.filter = filter.value; draw(); });
  draw();
  return el('div', { class: 'sy-chooser' }, el('div', { class: 'filter-row' }, filter), boxes,
    selectionEmpty(s) ? el('p', { class: 'sy-choose-some', role: 'status', text: t('syChooseSome') }) : null);
}

// ---- rendering: the running and finished states ------------------------------------------------

function activeCard() {
  switch (sy.state) {
    case 'planning': return planningCard();
    case 'syncing': return syncingCard();
    case 'building': return buildingCard();
    case 'paused': return pausedCard();
    case 'locked': return lockedCard();
    case 'done': case 'done_with_problems': case 'blocked_db': return resultCard();
    case 'error': return M.messageCard(M.ICONS.warn, C.errorMessage(sy.error), '',
      el('button', { class: 'btn', text: t('retry'), onclick: () => (sy.kinds ? startSync(sy.kinds) : backToIdle()) }),
      el('button', { class: 'btn link', text: t('ok'), onclick: () => backToIdle() }));
    case 'preview': return previewStub();
    default: return null;
  }
}

function renderActive() {
  const card = document.querySelector('.sy-active');
  if (!card) return;
  if (sy.state === 'planning') fillPlanning(card);
  else if (sy.state === 'syncing') fillSyncing(card);
}

function kindsText() {
  return sy.kinds && sy.kinds !== 'all' ? kindTitle(sy.kinds) : t('syAll');
}

function planningCard() {
  const card = el('div', { class: 'card lib-loading sy-active' });
  fillPlanning(card);
  return card;
}

function fillPlanning(card) {
  const sc = sy.progress.scan;
  let text = t('syReading', { what: kindsText() });
  let frac = null;
  if (sc?.op === 'tags' && sc.total > 0) { text = t('syReadingTags', { done: fmtCount(sc.done), total: fmtCount(sc.total) }); frac = sc.done / sc.total; }
  else if (sc?.op === 'list' || sc?.op === 'fingerprint') text = t('syComparing');
  card.replaceChildren(
    el('div', { class: 'lib-loading-art' }, el('span'), el('span'), el('span')),
    el('p', { class: 'lib-loading-text', text }),
    el('div', { class: `step-bar${frac === null ? ' wait' : ''}` }, el('span', { style: frac === null ? '' : `width:${(frac * 100).toFixed(1)}%` })));
}

// While the preview sheet is open, the tab shows what it is waiting for.
function previewStub() {
  return el('div', { class: 'card sy-waiting' }, M.icon(M.ICONS.info, 'i'), el('span', { class: 'grow', text: t('syPreviewOpen') }),
    el('button', { class: 'btn primary', text: t('syShowPreview'), onclick: () => openPreview() }));
}

function syncingCard() {
  const card = el('div', { class: 'card applying sy-active' });
  fillSyncing(card);
  return card;
}

function fillSyncing(card) {
  const p = sy.progress;
  const frac = p.total ? Math.min(1, p.done / p.total) : 0;
  const fileFrac = p.fileTotal ? Math.min(1, p.fileDone / p.fileTotal) : null;
  const cur = currentAlbum(p.file);
  let sub = '';
  if (p.reconnecting) sub = t('syReconnecting');
  else if (p.phase === 'move') sub = t('syMoving');
  else if (p.phase === 'remove') sub = t('syRemoving');
  else if (p.phase === 'pull') sub = t('syPulling', { name: cur?.name ?? '' });
  else if (cur?.album) sub = t('syAlbumNow', { album: cur.album, artist: cur.artist, name: cur.name });
  else if (cur) sub = cur.name;
  else sub = t('syStarting');
  const left = fmtLeft(secondsLeft(p.done, p.total, p.rate), t);
  const rate = p.rate ? t('rateText', { size: fmtBytes(p.rate) }) : '';
  const eta = secondsLeft(p.done, p.total, p.rate);
  const n = Math.min((p.files_done ?? 0) + 1, p.files_total ?? 0);
  card.replaceChildren(...[
    el('div', { class: 'applying-head' },
      el('div', { class: 'applying-icon' }, M.icon(M.ICONS.sync)),
      el('div', { class: 'grow' },
        el('h2', { text: p.files_total ? t('syCopying', { done: fmtCount(n), total: fmtCount(p.files_total) }) : t('syWorking', { what: kindsText() }) }),
        el('p', { class: 'muted', text: [p.total ? `${fmtBytes(p.done)} / ${fmtBytes(p.total)}` : '', rate, left].filter(Boolean).join(' · ') })),
      el('button', { class: 'btn', text: t('syPause'), onclick: () => pause() }),
      el('button', { class: 'btn', text: t('cancel'), onclick: () => stop() })),
    el('div', { class: 'step-bar big' }, el('span', { style: `width:${(frac * 100).toFixed(1)}%` })),
    el('div', { class: 'applying-file' },
      el('span', { class: 'applying-name', text: sub }),
      el('div', { class: `step-bar small${fileFrac === null ? ' wait' : ''}` }, el('span', { style: fileFrac === null ? '' : `width:${(fileFrac * 100).toFixed(1)}%` }))),
    fasterOverUsb(link(), eta) ? el('p', { class: 'sy-note', text: t('syFasterUsb') }) : null,
    p.warnings.some((w) => w.code === 'failed') ? el('p', { class: 'fail-text small-text', text: tc('syFailedSoFar', p.warnings.filter((w) => w.code === 'failed').length) }) : null,
  ].filter(Boolean));
}

function buildingCard() {
  return el('div', { class: 'card applying sy-active building' },
    el('div', { class: 'applying-head' },
      el('div', { class: 'applying-icon db' }, M.icon(M.ICONS.db)),
      el('div', { class: 'grow' },
        el('h2', { text: t('syBuilding') }),
        el('p', { class: 'muted', text: t('syBuildingBody') }))),
    el('div', { class: 'step-bar big wait' }, el('span')));
}

function pausedCard() {
  const reason = sy.pause?.reason ?? 'user';
  const p = sy.progress;
  const run = sy.pause?.run;
  const done = run ? run.done : p.files_done;
  const total = run ? null : p.files_total;
  const frac = p.total ? Math.min(1, p.done / p.total) : 0;
  return el('div', { class: `card applying sy-paused r-${reason}` },
    el('div', { class: 'applying-head' },
      el('div', { class: 'applying-icon paused' }, M.icon(reason === 'usb_storage' ? M.ICONS.sd : reason === 'battery' ? M.ICONS.battery : M.ICONS.pause)),
      el('div', { class: 'grow' },
        el('h2', { text: t(`syPaused_${reason}`) }),
        el('p', { class: 'muted', text: [t(`syPausedBody_${reason}`), total ? t('syDoneOf', { done: fmtCount(done), total: fmtCount(total), count: total }) : done ? tc('syDoneSoFar', done) : ''].filter(Boolean).join(' ') })),
      run?.run_id || sy.pause?.runId ? el('button', { class: 'btn link danger sy-discard', text: t('syDiscard'), onclick: () => discard() }) : null,
      el('button', { class: 'btn', text: t('syNotNow'), onclick: () => backToIdle() }),
      el('button', { class: 'btn primary', text: t('syResume'), disabled: !reachable(), onclick: () => resume() })),
    total ? el('div', { class: 'step-bar big paused' }, el('span', { style: `width:${(frac * 100).toFixed(1)}%` })) : null);
}

function lockedCard() {
  const by = sy.lockedBy ?? {};
  const v = lockView(by);
  const vars = { name: by.name || '?', age: by.heartbeat ? M.ageText(by.heartbeat) : '' };
  // another sync on this computer: only waiting helps (Take Over cannot pass this computer's own lock)
  const actions = v.takeOver
    ? [el('button', { class: 'btn', text: t('syNotNow'), onclick: () => backToIdle() }),
      el('button', { class: 'btn primary', text: t('syTakeOver'), onclick: () => takeOver() })]
    : [el('button', { class: 'btn', text: t('syTryAgain'), disabled: !reachable(), onclick: () => tryAgain() }),
      el('button', { class: 'btn primary', text: t('ok'), onclick: () => backToIdle() })];
  return el('div', { class: `card sy-locked${v.takeOver ? '' : ' here'}` },
    el('div', { class: 'applying-head' },
      el('div', { class: 'applying-icon lock' }, M.icon(M.ICONS.lock)),
      el('div', { class: 'grow' },
        el('h2', { text: t(v.title, vars) }),
        el('p', { class: 'muted', text: t(v.body, vars) })),
      ...actions));
}

function resultCard() {
  const f = sy.final ?? {};
  const s = sy.state;
  const tot = resultTotals(f);
  const ok = s === 'done';
  const card = el('div', { class: `card result ${ok ? 'ok' : s === 'done_with_problems' ? 'partial' : 'pending-db'}` });
  const title = s === 'done_with_problems' ? tc('syProblems', (f.failures ?? []).length || tot.failed) : s === 'blocked_db' ? t('syCopiedOnly') : t('syDone');
  card.append(el('div', { class: 'result-icon' }, M.icon(ok ? M.ICONS.check : s === 'done_with_problems' ? M.ICONS.warn : M.ICONS.info)), el('h2', { text: title }));
  const parts = [tc('syResCopied', tot.written, { size: fmtBytes(tot.bytes) })];
  if (tot.moved) parts.push(tc('syResMoved', tot.moved));
  if (tot.removed) parts.push(tc('syResRemoved', tot.removed));
  if (tot.pulled) parts.push(tc('syResPulled', tot.pulled));
  if (tot.kept) parts.push(tc('syResKept', tot.kept));
  card.append(el('p', { class: 'muted', text: parts.join(' · ') }));
  if (s === 'done') {
    card.append(el('p', { class: 'sy-lib-line', text: f.library?.restart ? t('syRestart') : f.library?.ok && f.library.action !== 'none' ? t('syLibUpdated') : '' }));
  }
  if (s === 'done_with_problems') {
    const ul = el('ul', { class: 'fail-list' });
    // the reason in words (BTP names such as E_IO through the app's error texts); the code stays in the tooltip
    for (const i of (f.failures ?? []).slice(0, 12)) {
      const why = C.errorMessage({ btp: i.error ? { name: i.error } : undefined, code: i.code, message: t('syFailGeneric') });
      ul.append(el('li', { title: [i.error, i.message].filter(Boolean).join(': ') }, el('span', { text: (i.dest ?? '').split('/').pop() }), el('span', { class: 'muted', text: why })));
    }
    card.append(el('p', { class: 'muted', text: t('syProblemsBody') }), ul);
  }
  if (s === 'blocked_db' || (s === 'done_with_problems' && f.library && f.library.action !== 'none' && !f.library.ok)) {
    card.append(el('div', { class: 'pending' }, M.icon(M.ICONS.info, 'i pending-i'),
      el('div', {}, el('p', { text: t('syLibraryPending') }),
        el('div', { class: 'howto' }, el('span', { class: 'howto-step', text: t('howtoSettings') }), el('span', { text: '›' }), el('span', { class: 'howto-step', text: t('howtoUpdate') })))));
  }
  const actions = el('div', { class: 'result-actions' });
  if (s === 'done_with_problems') actions.append(el('button', { class: 'btn primary', text: t(`syRetryFailed_${sy.kinds ?? 'all'}`), onclick: () => startSync(sy.kinds ?? 'all') }));
  actions.append(el('button', { class: `btn${s === 'done_with_problems' ? '' : ' primary'}`, text: t('syOk'), onclick: () => backToIdle() }));
  card.append(actions);
  return card;
}

// ---- the preview sheet ---------------------------------------------------------------------------

function openPreview() {
  const dlg = $('sync-preview');
  if (!dlg || !sy.plan) return;
  fillPreview();
  if (!dlg.open) dlg.showModal();
}

function closePreview() {
  const dlg = $('sync-preview');
  if (dlg?.open) dlg.close();
}

function cancelPreview() {
  closePreview();
  sy.state = 'idle';
  sy.plan = null;
  render();
}

const SECTION_ICON = { add: 'plus', update: 'refresh', move: 'move', player_only: 'sd', conflict: 'warn', blocked: 'block' };

function fillPreview() {
  const dlg = $('sync-preview');
  const plan = sy.plan;
  const ch = sy.choices;
  const groups = groupPlan(plan);
  const del = deletionSummary(plan, ch);
  const tot = planTotals(plan, ch);
  const okFit = fits(plan, ch);
  const empty = planIsEmpty(plan);
  const idle = nothingToDo(plan, ch);
  const eta = planEta(plan);
  const scroller = dlg.querySelector('.pv-body');
  const keep = scroller ? scroller.scrollTop : 0;
  dlg.replaceChildren();
  const head = el('header', { class: 'pv-head' },
    el('div', { class: 'grow' },
      el('h2', { text: t('pvTitle', { what: kindsText() }) }),
      el('p', { class: 'pv-headline', text: headline(plan, ch, t, fmt) })),
    el('button', { class: 'tool-btn pv-close', 'aria-label': t('cancel'), title: t('cancel'), onclick: () => cancelPreview() }, M.icon(M.ICONS.close)));
  const notes = el('div', { class: 'pv-notes' });
  for (const g of guards(plan)) {
    notes.append(el('div', { class: 'pv-banner danger' }, M.icon(M.ICONS.warn, 'i'),
      el('div', {}, el('strong', { text: g.reason === 'mass_delete' ? t('pvGuardMassTitle') : t(`syGuard_${g.kind}`) }),
        el('span', { text: g.reason === 'mass_delete' ? t('pvGuardMassBody') : t(`pvGuardBody_${g.reason}`) }))));
  }
  if (!okFit) notes.append(el('div', { class: 'pv-banner danger' }, M.icon(M.ICONS.warn, 'i'), el('div', {}, el('strong', { text: t('notEnoughSpace') }), el('span', { text: t('pvNoSpaceBody') }))));
  if (tot.needsDecision) notes.append(el('div', { class: 'pv-banner info' }, M.icon(M.ICONS.info, 'i'), el('span', { text: tc('pvNeedsDecision', tot.needsDecision) })));
  if (fasterOverUsb(link(), eta)) notes.append(el('div', { class: 'pv-banner info' }, M.icon(M.ICONS.info, 'i'), el('span', { text: t('syFasterUsbPlan', { min: Math.round(eta / 60) }) })));
  const body = el('div', { class: 'pv-body' });
  if (empty) body.append(el('div', { class: 'pv-empty' }, el('div', { class: 'result-icon ok-mini' }, M.icon(M.ICONS.check)), el('h3', { text: t('pvNothing') }), el('p', { class: 'muted', text: t('pvNothingBody') })));
  for (const g of groups) {
    const block = el('section', { class: 'pv-kind' });
    if (groups.length > 1) block.append(el('h3', { class: 'pv-kind-title' }, el('span', { class: `sy-kind-icon mini k-${g.kind}` }, M.icon(M.ICONS[KIND_ICON[g.kind]])), el('span', { text: kindTitle(g.kind) })));
    let any = false;
    for (const sec of ['add', 'update', 'move', 'player_only', 'conflict', 'blocked']) {
      const items = sec === 'player_only' ? null : g[sec];
      const n = sec === 'player_only' ? playerOnlyCount(g) : items.length;
      if (!n) continue;
      any = true;
      block.append(section(g, sec, n, items));
    }
    if (g.quiet) block.append(el('p', { class: 'muted small-text pv-quiet', text: tc('pvQuiet', g.quiet) }));
    if (!any && !g.quiet && groups.length > 1) block.append(el('p', { class: 'muted small-text pv-quiet', text: t('pvKindNothing') }));
    body.append(block);
  }
  const foot = el('footer', { class: 'pv-foot' });
  // what is deleted, and on a second line what cannot be recovered (files not on this computer)
  const delText = el('div', { class: `pv-del grow${del.count ? ' danger' : ''}` },
    el('span', { text: del.count ? tc('pvDelLine', del.count, { size: fmtBytes(del.bytes) }) : t('pvNoDelete') }));
  if (del.lost) delText.append(el('span', { class: 'pv-del-lost', text: del.lost === del.count ? tc('pvDelLostAll', del.lost) : tc('pvDelLost', del.lost) }));
  foot.append(delText);
  // nothing would change: one Done button (the header's close button stays)
  if (idle) foot.append(el('button', { class: 'btn primary pv-apply', text: t('syOk'), onclick: () => cancelPreview() }));
  else {
    foot.append(el('button', { class: 'btn', text: t('cancel'), onclick: () => cancelPreview() }));
    foot.append(el('button', {
      class: `btn primary${del.count ? ' danger' : ''} pv-apply`, text: applyLabel(plan, ch, t, fmt),
      disabled: !okFit || !reachable(),
      onclick: () => apply(),
    }));
  }
  dlg.append(head, notes, body, foot);
  const nb = dlg.querySelector('.pv-body');
  if (nb) nb.scrollTop = keep;
}

function section(g, sec, n, items) {
  const key = `${g.kind}:${sec}`;
  const open = sy.open.has(sec) ? !sy.open.has(`-${key}`) : sy.open.has(key);
  const bytes = items ? items.reduce((s, i) => s + (i.size ?? 0), 0) : 0;
  const right = sec === 'add' || sec === 'update' ? fmtBytes(bytes) : '';
  const s = el('div', { class: `pv-sec s-${sec}${open ? ' open' : ''}` });
  const hdr = el('button', {
    class: 'pv-sec-head', 'aria-expanded': String(open),
    onclick: () => {
      if (sy.open.has(sec)) { if (sy.open.has(`-${key}`)) sy.open.delete(`-${key}`); else sy.open.add(`-${key}`); }
      else if (sy.open.has(key)) sy.open.delete(key); else sy.open.add(key);
      fillPreview();
    },
  },
  el('span', { class: 'chev', 'aria-hidden': 'true' }),
  el('span', { class: `pv-sec-icon s-${sec}` }, M.icon(M.ICONS[SECTION_ICON[sec]])),
  el('span', { class: 'pv-sec-title', text: t(`pvSec_${sec}`, { n: fmtCount(n) }) }),
  el('span', { class: 'spacer' }),
  el('span', { class: 'pv-sec-meta', text: right }));
  s.append(hdr);
  if (!open) return s;
  if (sec === 'player_only') {
    for (const r of REASONS) {
      const list = g.player_only[r] ?? [];
      if (list.length) s.append(reasonGroup(g, r, list));
    }
    return s;
  }
  s.append(rows(items, (it) => itemRow(sec, it)));
  return s;
}

function reasonGroup(g, reason, list) {
  const gc = groupChoice(sy.choices, list);
  const v = groupView(sy.plan, reason, list);
  const seg = el('div', { class: 'segmented small pv-group-seg', role: 'radiogroup', 'aria-label': t(v.title) });
  // Delete All is not offered when the folder could not be read (each file can still be deleted,
  // behind the typed count); Copy All to Computer when the files can be pulled
  const opts = [['keep', 'pvKeepAll'], ...(v.deleteAll ? [['delete', 'pvDeleteAll']] : []), ...(v.pullAll ? [['pull', 'pvCopyAll']] : [])];
  for (const [c, label] of opts) {
    seg.append(el('button', {
      role: 'radio', 'aria-selected': String(gc === c), 'aria-checked': String(gc === c), class: c === 'delete' ? 'del' : '',
      text: t(label),
      onclick: () => { setGroup(sy.choices, list, c); fillPreview(); },
    }));
  }
  const guarded = list.some((i) => i.guard);
  return el('div', { class: `pv-group${guarded ? ' guarded' : ''}` },
    el('div', { class: 'pv-group-head' },
      el('div', { class: 'grow' },
        el('div', { class: 'pv-group-title', text: `${t(v.title)} · ${fmtCount(list.length)}` }),
        el('div', { class: 'muted small-text', text: t(v.hint) }),
        v.lost ? el('div', { class: 'small-text pv-lost-hint', text: t('pvReasonHintLost') }) : null),
      opts.length > 1 ? seg : null),
    rows(list, (it) => playerOnlyRow(it)));
}

// A list of rows; long ones (tens of thousands of files) only draw what is in view.
function rows(items, draw) {
  const box = el('div', { class: 'pv-rows' });
  if (items.length <= 60) {
    for (const it of items) box.append(draw(it));
    return box;
  }
  box.classList.add('virtual');
  const spacer = el('div', { class: 'pv-spacer', style: `height:${items.length * ROW_H}px` });
  const layer = el('div', { class: 'pv-layer' });
  spacer.append(layer);
  box.append(spacer);
  const paint = () => {
    const w = virtualWindow(box.scrollTop, box.clientHeight || 330, ROW_H, items.length, 6);
    layer.style.transform = `translateY(${w.top}px)`;
    layer.replaceChildren(...items.slice(w.start, w.end).map(draw));
  };
  box.addEventListener('scroll', () => requestAnimationFrame(paint), { passive: true });
  requestAnimationFrame(paint);
  paint();
  return box;
}

// A text for a reason byos-cli may add later: its own key, else a general one.
function known(key, fallback) {
  const s = t(key);
  return s === key ? t(fallback) : s;
}

function destText(it) {
  return it.action === 'move' && it.old_dest ? `${it.old_dest} → ${it.dest}` : it.dest;
}

function itemRow(sec, it) {
  const name = it.title || (it.dest ?? '').split('/').pop();
  const row = el('div', { class: 'pv-row', title: destText(it) },
    el('span', { class: 'pv-name', text: name }),
    el('span', { class: 'pv-path', text: destText(it) }));
  if (sec === 'conflict') {
    const seg = el('div', { class: 'segmented small pv-row-seg', role: 'radiogroup', 'aria-label': name });
    for (const v of it.choices ?? []) {
      seg.append(el('button', {
        role: 'radio', 'aria-selected': String(sy.choices.get(it.id) === v), 'aria-checked': String(sy.choices.get(it.id) === v), text: t(`pvConflict_${v}`), title: t(`pvConflictHint_${v}`),
        onclick: () => { setChoice(sy.choices, it, v); fillPreview(); },
      }));
    }
    row.append(el('span', { class: 'pv-reason', text: known(`pvConflictWhy_${it.reason}`, 'pvConflictWhy_other') }), seg);
  } else if (sec === 'blocked') {
    row.append(el('span', { class: 'pv-reason warn', text: known(`pvBlocked_${it.reason}`, String(it.reason).startsWith('bad_path_') ? 'pvBlocked_bad_path' : 'pvBlocked_other') }));
  } else {
    row.append(el('span', { class: 'pv-size', text: fmtBytes(it.size) }));
  }
  return row;
}

function playerOnlyRow(it) {
  const name = it.title || (it.dest ?? '').split('/').pop();
  const c = sy.choices.get(it.id) ?? 'keep';
  const sel = el('select', { class: `pv-choice c-${c}`, 'aria-label': name });
  for (const v of it.choices ?? ['keep', 'delete']) sel.append(el('option', { value: v, text: t(`pvChoice_${v}`), selected: v === c }));
  sel.addEventListener('change', () => { setChoice(sy.choices, it, sel.value); fillPreview(); });
  return el('div', { class: `pv-row po${c === 'delete' ? ' del' : ''}${it.needs_decision && c === 'keep' ? ' undecided' : ''}`, title: it.dest },
    el('span', { class: 'pv-name', text: name }),
    el('span', { class: 'pv-path', text: it.dest }),
    el('span', { class: 'pv-size', text: fmtBytes(it.size) }),
    sel);
}

// ---- wiring --------------------------------------------------------------------------------------

export function initSync(ctx, media) {
  C = ctx;
  M = media;
  const dlg = $('sync-preview');
  dlg?.addEventListener('cancel', (e) => { e.preventDefault(); cancelPreview(); });
}

export function syncTabOpened() {
  render();
  loadLinks();
}

export function syncDeviceChanged() {
  if (busy()) return;
  sy.deviceId = null;
  sy.links = null;
  sy.catalog = null;
}

// The computer's library asks for a sync of one kind (its toolbar's Sync button).
export function syncKind(kind) {
  if (!busy()) startSync(kind);
}

export function syncState() { return sy; }

// ---- demo mode (?demo=sync-NAME): the real mock flow up to a state, for screenshots -----------------

export async function demoSync(name, params) {
  sy.demo = { name, params };
  // the Music tab's folders, before the first visit of the Sync tab: migrated into links
  if (name === 'sync-migrate') {
    try { await C.api('/api/pick-folder', { method: 'POST', body: {} }); } catch { /* demo */ }
    try { localStorage.setItem('byos.music.sel', JSON.stringify({ all: false, artists: ['aurora fields'], albums: [], genres: ['jazz'] })); } catch { /* demo */ }
  }
  return 'sync';
}

// Called once the links are loaded: drives the flow to the demo's state.
export async function demoSyncStep() {
  const d = sy.demo;
  if (!d || sy.links === null || sy.loading) return;
  sy.demo = null;
  const name = d.name;
  const kind = d.params.get('kind') ?? 'music';
  const linkAll = async () => {
    for (const k of (d.params.get('kinds') ?? 'music,audiobooks,ebooks,video').split(',')) {
      if (linksOf(sy.links, k).length) continue;
      const r = await C.api('/api/links/pick', { method: 'POST', body: { kind: k } });
      await C.runCli('link-add', { deviceId: sy.deviceId, kind: k, folder: r.folder, ...(k === 'music' ? { layout: 'artist-album' } : {}) }, { quiet: true });
    }
    await loadLinks({ force: true });
  };
  if (name === 'sync-empty') return;
  if (name === 'sync-migrate') { for (const l of linksOf(sy.links, 'music')) sy.expanded.add(l.id); render(); return; }
  await linkAll();
  if (d.params.get('policy')) {
    for (const l of linksOf(sy.links, 'music')) await C.runCli('link-set', { deviceId: sy.deviceId, id: l.id, playerOnly: d.params.get('policy') }, { quiet: true });
    await loadLinks({ force: true });
  }
  if (d.params.get('off')) {
    for (const l of linksOf(sy.links, d.params.get('off'))) await C.runCli('link-set', { deviceId: sy.deviceId, id: l.id, enabled: false }, { quiet: true });
    await loadLinks({ force: true });
  }
  if (d.params.get('options')) { for (const l of linksOf(sy.links, d.params.get('options'))) sy.expanded.add(l.id); render(); }
  if (d.params.get('chosen')) {
    for (const l of linksOf(sy.links, 'music')) {
      await C.runCli('link-set', { deviceId: sy.deviceId, id: l.id, select: d.params.get('chosen').split('|') }, { quiet: true });
      sy.expanded.add(l.id);
    }
    await C.runCli('music-scan', {}, { quiet: true });
    await loadLinks({ force: true });
    await loadCatalog();
  }
  if (d.params.get('nochoice')) {
    // "Selected artists, albums and genres" with nothing ticked: that link cannot sync
    for (const l of linksOf(sy.links, 'music')) {
      sy.pendingSel.set(l.id, { all: false, artists: new Set(), albums: new Set(), genres: new Set() });
      sy.expanded.add(l.id);
    }
    render();
  }
  if (name === 'sync-idle') { await waitFor(() => !sy.checking && sy.checkedAt); render(); return; }
  await waitFor(() => !sy.checking);
  const freeze = d.params.get('freeze');
  if (d.params.get('wifi')) sy.demoWifi = true;
  if (name === 'sync-planning') {
    sy.kinds = kind; sy.state = 'planning'; sy.progress = emptyProgress();
    sy.progress.scan = { op: 'tags', done: 1240, total: 3900 };
    render();
    return;
  }
  await startSync(kind);
  if (sy.demoWifi && sy.plan) { for (const k of Object.keys(sy.plan.summary ?? {})) sy.plan.summary[k].eta_s *= 5; fillPreview(); }
  if (name === 'sync-preview') {
    if (d.params.get('only') !== null) sy.open = new Set(d.params.get('only').split(',').filter(Boolean));
    for (const s of (d.params.get('open') ?? '').split(',').filter(Boolean)) sy.open.add(s);
    if (d.params.get('choose')) {
      for (const pair of d.params.get('choose').split(',')) {
        const [reason, v] = pair.split(':');
        setGroup(sy.choices, sy.plan.items.filter((i) => i.reason === reason && !i.parent), v);
      }
    }
    fillPreview();
    if (d.params.get('typed')) {
      await new Promise((r) => setTimeout(r, 200));
      apply();
      setTimeout(() => {
        const i = document.querySelector('#sheet input');
        if (i) i.value = d.params.get('typed');
        if (d.params.get('submit')) document.getElementById('sheet-ok')?.click();
      }, 300);
    }
    return;
  }
  if (name === 'sync-syncing' || name === 'sync-building' || name === 'sync-paused') {
    if (freeze) {
      closePreview();
      const tot = planTotals(sy.plan, sy.choices);
      Object.assign(sy.progress, emptyProgress(), {
        phase: 'put', files_total: tot.add + tot.update, files_done: 41, total: tot.bytesAdd, done: Math.round(tot.bytesAdd * 0.23), rate: sy.demoWifi ? 2300000 : 11200000,
        file: sy.plan.items.find((i) => i.action === 'add')?.dest, fileDone: 21000000, fileTotal: 33000000,
      });
      sy.state = name === 'sync-building' ? 'building' : name === 'sync-paused' ? 'paused' : 'syncing';
      if (name === 'sync-paused') sy.pause = { reason: d.params.get('reason') ?? 'user', runId: 'demo-run' };
      render();
      return;
    }
  }
  if (['sync-run', 'sync-paused', 'sync-locked', 'sync-result'].includes(name)) {
    if (d.params.get('choose')) {
      for (const pair of d.params.get('choose').split(',')) {
        const [reason, v] = pair.split(':');
        setGroup(sy.choices, sy.plan.items.filter((i) => i.reason === reason && !i.parent), v);
      }
    }
    await apply();
    if (d.params.get('resume') && sy.state === 'paused') await resume();
    if (d.params.get('takeover') && sy.state === 'locked') await takeOver();
  }
}

async function waitFor(cond, ms = 8000) {
  const t0 = Date.now();
  while (!cond() && Date.now() - t0 < ms) await new Promise((r) => setTimeout(r, 50));
}
