// The Sync tab's rules (docs/SYNC_LIBRARY_DESIGN.md section 10.2), shared by the page
// (/sync-flow.js) and the tests: the kinds, the migration of the Music tab's folders and
// selection into music links, the preview's sections and groups, the user's choices and the
// DEC.json they become, the deletion summary (with the typed mass-delete confirmation), how a
// sync-apply run ended, and its progress. No DOM here.

export const KINDS = ['music', 'audiobooks', 'ebooks', 'video'];
export const CARD_ROOTS = { music: 'Music', audiobooks: 'Audiobooks', ebooks: 'Books', video: 'Video' };
export const POLICIES = ['keep', 'delete', 'ask'];
// Player-only groups, in the order the preview shows them (section 3.3.2).
export const REASONS = ['deleted_on_computer', 'excluded_on_computer', 'from_other_device', 'added_on_player', 'downloaded_on_player'];
// The preview's sections, in order. "quiet" items (adopt, record) are only counted.
export const SECTIONS = ['add', 'update', 'move', 'player_only', 'conflict', 'blocked'];
export const QUIET = new Set(['adopt', 'record']);

// The links of one kind, in the order they were added.
export function linksOf(links, kind) {
  return (links ?? []).filter((l) => l.kind === kind);
}

// The folder's own name for a link or a source ("D:\Music" → "Music"); the whole path is a tooltip.
export function folderName(p) {
  if (typeof p !== 'string' || !p) return '';
  const clean = p.replace(/[\\/]+$/, '');
  return clean.split(/[\\/]/).pop() || clean;
}

export function linkPath(link) {
  const l = link?.local;
  return typeof l === 'string' ? l : l?.path ?? '';
}

// ---- migration from the Music tab (design section 2.1) ----------------------------------------

// The --select values of a Music tab selection (keys are folded names, as the tab kept them;
// byos-cli folds names before comparing).
export function selectArgs(sel) {
  if (!sel || sel.all !== false) return [];
  const out = [];
  for (const a of sel.artists ?? []) out.push(`artist:${a}`);
  for (const k of sel.albums ?? []) {
    const [artist, title] = String(k).split('\u0000');
    if (title !== undefined) out.push(`album:${artist}::${title}`);
  }
  for (const g of sel.genres ?? []) out.push(`genre:${g}`);
  return out;
}

// The link-add requests that turn the Music tab's folders (the runner's settings.sources, each
// with an opaque folder token) and its saved selection and options into music links. Folders that
// are gone, dropped-file folders, and folders already linked are left out. The first folder takes
// the Music tab's destination; byos-cli puts the others under Music/<label>.
export function migrationPlan(sources, sel, opts, links) {
  const linked = new Set((links ?? []).filter((l) => l.kind === 'music').map((l) => linkPath(l).replace(/[\\/]+$/, '').toLowerCase()));
  const out = [];
  const select = selectArgs(sel);
  for (const s of sources ?? []) {
    if (!s || s.temp || s.exists === false || !s.folder) continue;
    if (linked.has(String(s.path ?? '').replace(/[\\/]+$/, '').toLowerCase())) continue;
    const req = { kind: 'music', folder: s.folder, layout: opts?.layout === 'keep' ? 'keep' : 'artist-album', playerOnly: 'keep' };
    if (select.length) req.select = select;
    if (opts?.remove) req.removeUnselected = true;
    if (!out.length && opts?.dest && opts.dest !== 'Music') req.cardRoot = opts.dest;
    out.push(req);
  }
  return out;
}

// ---- the preview ----------------------------------------------------------------------------------

// The plan's items as the preview shows them, per kind: { kind, sections: { add: [...], ...,
// player_only: { reason: [...] } }, quiet, children } (children: sidecars that follow a parent).
export function groupPlan(plan) {
  const kinds = new Map();
  const get = (k) => {
    if (!kinds.has(k)) {
      kinds.set(k, {
        kind: k, add: [], update: [], move: [], conflict: [], blocked: [],
        player_only: Object.fromEntries(REASONS.map((r) => [r, []])), quiet: 0, children: 0,
      });
    }
    return kinds.get(k);
  };
  for (const k of plan?.kinds ?? []) get(k);
  for (const it of plan?.items ?? []) {
    const g = get(it.kind ?? 'music');
    if (it.parent) { g.children++; continue; }
    if (QUIET.has(it.action)) { g.quiet++; continue; }
    if (it.action === 'player_only') {
      (g.player_only[it.reason] ??= []).push(it);
    } else if (g[it.action]) g[it.action].push(it);
  }
  return [...kinds.values()].sort((a, b) => KINDS.indexOf(a.kind) - KINDS.indexOf(b.kind));
}

export function playerOnlyCount(g) {
  return Object.values(g.player_only).reduce((s, a) => s + a.length, 0);
}

// The starting choices: what byos-cli pre-ticked (an undecided item is kept).
export function initialChoices(plan) {
  const c = new Map();
  for (const it of plan?.items ?? []) {
    if (it.parent || !Array.isArray(it.choices) || !it.choices.length) continue;
    c.set(it.id, it.default_choice ?? (it.action === 'conflict' ? 'skip' : 'keep'));
  }
  return c;
}

export function setChoice(choices, item, choice) {
  if (!item || !item.choices?.includes(choice)) return false;
  choices.set(item.id, choice);
  return true;
}

// Keep or Delete for a whole group; items that cannot take the choice keep theirs.
export function setGroup(choices, items, choice) {
  let n = 0;
  for (const it of items) if (setChoice(choices, it, choice)) n++;
  return n;
}

// The group's choice as the group control shows it: "delete", "keep", or "mixed".
export function groupChoice(choices, items) {
  let seen = null;
  for (const it of items) {
    const c = choices.get(it.id) ?? 'keep';
    const v = c === 'delete' ? 'delete' : 'keep';
    if (seen === null) seen = v;
    else if (seen !== v) return 'mixed';
  }
  return seen ?? 'keep';
}

// DEC.json for sync-apply: every item whose choice differs from the pre-ticked one, plus every
// undecided item (so the run does exactly what the preview showed).
export function decisionsFor(plan, choices) {
  const out = {};
  for (const it of plan?.items ?? []) {
    if (it.parent || !choices.has(it.id)) continue;
    const c = choices.get(it.id);
    if (c !== it.default_choice || it.default_choice === null || it.default_choice === undefined) out[it.id] = c;
  }
  return out;
}

// What will be deleted from the HiBy: count, bytes, per reason, and the deletions DeleteGuard
// protects (the folder looks unavailable or many files would go). Those need the typed count.
export function deletionSummary(plan, choices) {
  const s = { count: 0, bytes: 0, reasons: {}, guarded: 0, guardedBytes: 0 };
  for (const it of plan?.items ?? []) {
    if (it.parent || it.action !== 'player_only' || choices.get(it.id) !== 'delete') continue;
    s.count++;
    s.bytes += it.size ?? 0;
    s.reasons[it.reason] = (s.reasons[it.reason] ?? 0) + 1;
    if (it.guard === true) { s.guarded++; s.guardedBytes += it.size ?? 0; }
  }
  return s;
}

export function needsTypedConfirm(del) {
  return (del?.guarded ?? 0) > 0;
}

// The typed confirmation: the count, written with or without group separators.
export function typedCountOk(text, count) {
  return String(text ?? '').replace(/[\s.,\u00a0\u202f']/g, '') === String(count);
}

// Copies left to do, in the plan's own terms.
export function planTotals(plan, choices = initialChoices(plan)) {
  const t = { add: 0, update: 0, move: 0, conflict: 0, blocked: 0, player_only: 0, quiet: 0, pull: 0, bytesAdd: 0, bytesNew: 0, needsDecision: 0 };
  for (const it of plan?.items ?? []) {
    if (it.parent) continue;
    if (QUIET.has(it.action)) { t.quiet++; continue; }
    if (it.action in t) t[it.action]++;
    if (it.action === 'add') { t.bytesAdd += it.size ?? 0; t.bytesNew += it.size ?? 0; }
    if (it.action === 'update' || (it.action === 'move' && it.then_update)) t.bytesAdd += Math.max(0, (it.size ?? 0) - Math.max(0, it.card_size ?? 0));
    if (it.action === 'player_only' && choices.get(it.id) === 'pull') t.pull++;
    if (it.needs_decision && choices.get(it.id) === (it.default_choice ?? 'keep')) t.needsDecision++;
  }
  return t;
}

// Free space on the card after the run, with the user's deletions.
export function freeAfter(plan, choices) {
  const del = deletionSummary(plan, choices);
  return Math.max(0, (plan?.sd_free ?? 0) - planTotals(plan, choices).bytesAdd + del.bytes);
}

export function fits(plan, choices, margin = 64 * 1048576) {
  if (!plan) return false;
  const del = deletionSummary(plan, choices);
  return planTotals(plan, choices).bytesAdd - del.bytes + margin <= (plan.sd_free ?? 0);
}

// Nothing to show: the plan lists no file at all.
export function planIsEmpty(plan) {
  return !(plan?.items ?? []).some((it) => !it.parent);
}

// Nothing would change on the HiBy with these choices (every file kept, every conflict skipped).
export function nothingToDo(plan, choices) {
  const t = planTotals(plan, choices);
  if (t.add + t.update + t.move + t.pull + t.quiet > 0 || deletionSummary(plan, choices).count > 0) return false;
  return !(plan?.items ?? []).some((it) => !it.parent && it.action === 'conflict' && (choices.get(it.id) ?? 'skip') !== 'skip');
}

// The tripped DeleteGuards of a plan, per link: [{ link, kind, label, reason }].
export function guards(plan) {
  return (plan?.links ?? []).filter((l) => l.guard?.tripped).map((l) => ({ link: l.id, kind: l.kind, label: l.label, reason: l.guard.reason }));
}

// The preview's header line: "Add 312 songs (2.1 GB) · Update 4 · Move 18 · Remove 12 · 1.2 GB free after".
export function headline(plan, choices, t, fmt) {
  const tot = planTotals(plan, choices);
  const del = deletionSummary(plan, choices);
  const kinds = plan?.kinds ?? [];
  const noun = kinds.length === 1 ? kinds[0] : 'all';
  const parts = [];
  if (tot.add) parts.push(t(`pvHeadAdd_${noun}`, { n: fmt.count(tot.add), size: fmt.bytes(tot.bytesNew) }));
  if (tot.update) parts.push(t('pvHeadUpdate', { n: fmt.count(tot.update) }));
  if (tot.move) parts.push(t('pvHeadMove', { n: fmt.count(tot.move) }));
  if (del.count) parts.push(t('pvHeadRemove', { n: fmt.count(del.count) }));
  if (!parts.length) parts.push(t('pvHeadNothing'));
  parts.push(t('pvHeadFree', { size: fmt.bytes(freeAfter(plan, choices)) }));
  return parts.join(' · ');
}

// The Apply button says what it deletes in plain words.
export function applyLabel(plan, choices, t, fmt) {
  const del = deletionSummary(plan, choices);
  if (del.count) return t('pvApplyDelete', { n: fmt.count(del.count) });
  return t('pvApply');
}

// ---- how a run ended ------------------------------------------------------------------------------

// The Sync tab's state after sync-apply or sync-resume: done, done_with_problems, paused (with a
// reason: link, usb_storage, battery, user), locked, blocked_db (copied, the HiBy's library is
// not updated), or error.
export function outcome(final, { userPaused = false } = {}) {
  if (!final) return { state: 'error' };
  if (!final.ok) {
    if (final.code === 'locked') return { state: 'locked', by: final.locked_by ?? null };
    if (final.code === 'cancelled') return userPaused || final.resumable ? { state: 'paused', reason: 'user' } : { state: 'cancelled' };
    if (final.btp?.name === 'E_BATTERY') return { state: 'paused', reason: 'battery' };
    if (final.btp?.name === 'E_SD_UNAVAILABLE') return { state: 'paused', reason: 'usb_storage' };
    if (final.code === 'no_device' || final.code === 'timeout') return { state: 'paused', reason: 'link' };
    return { state: 'error' };
  }
  if (final.resumed === false && final.status === 'nothing_to_resume') return { state: 'idle' };
  if (final.status === 'paused') return { state: 'paused', reason: 'link' };
  if (final.status === 'paused_usb_storage') return { state: 'paused', reason: 'usb_storage' };
  if (final.status === 'done_with_problems') return { state: 'done_with_problems' };
  const lib = final.library;
  if (lib && lib.action && lib.action !== 'none' && !lib.ok) return { state: 'blocked_db' };
  return { state: 'done', restart: !!lib?.restart };
}

// Totals of a sync-apply result over its kinds.
export function resultTotals(final) {
  const s = { written: 0, skipped: 0, moved: 0, removed: 0, pulled: 0, recorded: 0, failed: 0, not_fitted: 0, blocked: 0, conflicts_left: 0, kept: 0, bytes: 0 };
  for (const k of Object.values(final?.kinds ?? {})) for (const f of Object.keys(s)) s[f] += k[f] ?? 0;
  return s;
}

// ---- progress -----------------------------------------------------------------------------------

export function emptyProgress() {
  return { phase: 'prepare', done: 0, total: 0, rate: 0, files_done: 0, files_total: 0, file: null, fileDone: 0, fileTotal: 0, warnings: [] };
}

// One byos-cli line into the progress the syncing card shows.
export function applyProgress(p, ev) {
  if (ev.event === 'warn') {
    if (['failed', 'no_space', 'reconnecting', 'resumed', 'journal_replayed', 'low_battery', 'library_pending'].includes(ev.code)) p.warnings.push(ev);
    if (ev.code === 'reconnecting') p.reconnecting = true;
    return p;
  }
  if (ev.event !== 'progress') return p;
  p.reconnecting = false;
  switch (ev.op) {
    case 'sync':
      Object.assign(p, { done: ev.done ?? p.done, total: ev.total ?? p.total, files_done: ev.files_done ?? p.files_done, files_total: ev.files_total ?? p.files_total });
      if (ev.rate) p.rate = ev.rate;
      if (ev.path) p.file = ev.path;
      if (p.phase !== 'library') p.phase = 'put';
      break;
    case 'put': case 'hash': case 'prepare':
      Object.assign(p, { file: ev.path ?? p.file, fileDone: ev.done ?? 0, fileTotal: ev.total ?? 0, phase: ev.op === 'put' ? 'put' : 'prepare' });
      break;
    case 'move': p.phase = 'move'; p.file = ev.path ?? p.file; break;
    case 'remove': p.phase = 'remove'; p.file = ev.path ?? p.file; break;
    case 'pull': p.phase = 'pull'; p.file = ev.path ?? p.file; p.fileDone = ev.done ?? 0; p.fileTotal = ev.total ?? 0; break;
    case 'library': case 'db_build': case 'db_push': case 'db_verify': case 'rescan': p.phase = 'library'; break;
    case 'scan': case 'tags': case 'list': case 'fingerprint': p.phase = 'scan'; p.scan = { op: ev.op, done: ev.done ?? 0, total: ev.total ?? 0 }; break;
    default: break;
  }
  return p;
}

// The album a card path is in ("Music/ABBA/Gold/03 SOS.flac" → { album: "Gold", artist: "ABBA", name: "03 SOS" }).
export function currentAlbum(dest) {
  if (!dest) return null;
  const parts = String(dest).split('/');
  const file = parts.pop() ?? '';
  return { name: file.replace(/\.[^.]+$/, ''), album: parts.length > 1 ? parts[parts.length - 1] : '', artist: parts.length > 2 ? parts[parts.length - 2] : '' };
}

// "Faster over USB": on Wi-Fi when the run would take more than 30 minutes.
export function fasterOverUsb(device, etaSeconds) {
  return !!device && device.transport === 'wifi' && (etaSeconds ?? 0) > 1800;
}

// The plan's own estimate of the time, summed over its kinds.
export function planEta(plan) {
  return Object.values(plan?.summary ?? {}).reduce((s, k) => s + (k.eta_s ?? 0), 0);
}

// ---- the idle line of each kind ("3 changes on this computer") ------------------------------------

export function kindChanges(summary) {
  if (!summary) return null;
  const po = summary.player_only ?? {};
  const deleted = po.deleted_on_computer ?? 0;
  return (summary.add ?? 0) + (summary.update ?? 0) + (summary.move ?? 0) + deleted;
}
