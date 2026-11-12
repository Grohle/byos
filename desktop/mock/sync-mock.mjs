// The fake byos-cli's linked-folder sync (docs/BYOS_CLI.md "Folder sync", design sections 3
// and 9): link-ls/add/set/rm, sync-scan, sync-plan, sync-apply, sync-resume, sync-status and
// sync-unlock, with made-up plans that show every part of the Sync tab's preview. Links live
// where byos-cli keeps them (<config>/sync/<device>/links.json), so the runner finds the
// computer's music folders there. Scenarios (BYOS_MOCK_SCENARIO):
//   sync_unavailable   the music folder is missing: DeleteGuard keeps every deletion unticked
//   sync_mass_delete   240 files deleted on this computer: DeleteGuard asks for the typed count
//   sync_locked        another computer holds the HiBy's sync lock (Take over works)
//   sync_pause         the link drops after 40 % of the copies (paused; sync-resume goes on)
//   sync_usb_pause     the card becomes a USB drive during the copies (paused_usb_storage)
//   sync_partial       three files fail (done_with_problems)
//   sync_db_pending    the HiBy cannot update its library from here (library_pending)
//   sync_battery       the HiBy's battery is too low to start
//   card_full          the card has no room for the plan
//   ask_policy         new links ask about files only on the HiBy

import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { hibyIndex, localCatalog } from './library-fixture.mjs';

const KINDS = ['music', 'audiobooks', 'ebooks', 'video'];
const ROOTS = { music: 'Music', audiobooks: 'Audiobooks', ebooks: 'Books', video: 'Video' };
const CLIENT_ID = 'c0ffee0123456789';
const fold = (s) => String(s ?? '').normalize('NFD').replace(/\p{M}+/gu, '').toLowerCase();
const pad = (n) => String(n).padStart(2, '0');
const clean = (s) => String(s).replace(/[\\/:*?"<>|]/g, '_').replace(/[. ]+$/, '');
const hex = (n) => Array.from({ length: n }, () => Math.floor(Math.random() * 16).toString(16)).join('');

export function installSync(commands, ctx) {
  const { opts, emit, Fail, tick, connect, requireSd, load, save, dfFields, nextAnswer, SCENARIO, DEVICE_ID, configDir, cachePath, isCancelled } = ctx;
  // BYOS_MOCK_SYNC_STEP_MS: the pace of the fake scan and copies (default 6 ms; slow: 90; tests: 0)
  const STEP = Number(process.env.BYOS_MOCK_SYNC_STEP_MS ?? (SCENARIO === 'slow' ? 90 : 6));

  const devId = () => {
    if (opts['device-id']) return String(opts['device-id']).toLowerCase();
    connect();
    return DEVICE_ID;
  };
  const linksFile = (dev) => path.join(configDir(), 'sync', dev, 'links.json');
  const loadLinks = (dev) => {
    try { return JSON.parse(readFileSync(linksFile(dev), 'utf8')); } catch { return { format: 1, client_id: CLIENT_ID, client_name: 'THIS-PC', links: [] }; }
  };
  const saveLinks = (dev, lf) => {
    const f = linksFile(dev);
    mkdirSync(path.dirname(f), { recursive: true });
    writeFileSync(`${f}.tmp`, JSON.stringify(lf, null, 1));
    renameSync(`${f}.tmp`, f);
  };
  const syncState = (state) => {
    state.sync ??= { applied: {}, runs: [], lock: null, lastSync: {} };
    return state.sync;
  };

  function applyOptions(l) {
    if (opts['card-root']) l.card_root = opts['card-root'];
    if (opts.layout) {
      if (!['artist-album', 'keep'].includes(opts.layout)) throw new Fail('usage', '--layout is artist-album or keep');
      if (opts.layout === 'artist-album' && l.kind !== 'music') throw new Fail('usage', 'the artist-album layout is for music links');
      l.layout = opts.layout;
    }
    if (opts['player-only']) {
      if (!['keep', 'delete', 'ask'].includes(opts['player-only'])) throw new Fail('usage', '--player-only is keep, delete or ask');
      l.player_only = opts['player-only'];
    }
    if (opts.covers) l.covers = opts.covers;
    if (opts.label) l.label = opts.label;
    if (opts['remove-unselected']) l.remove_unselected = true;
    if (opts['keep-unselected']) l.remove_unselected = false;
    if (opts.disable) l.enabled = false;
    if (opts.enable) l.enabled = true;
    if (opts['select-all']) l.selection = { mode: 'all', artists: [], albums: [], genres: [], files: [] };
    const sel = opts.selectList ?? [];
    if (sel.length) {
      if (l.kind !== 'music') throw new Fail('usage', 'choosing artists, albums or genres is for music links');
      const s = { mode: 'chosen', artists: [], albums: [], genres: [], files: [] };
      for (const v of sel) {
        const k = v.slice(0, v.indexOf(':'));
        const x = v.slice(v.indexOf(':') + 1);
        if (k === 'artist') s.artists.push(x);
        else if (k === 'genre') s.genres.push(x);
        else if (k === 'album') s.albums.push({ artist: x.split('::')[0], title: x.split('::')[1] ?? '' });
        else throw new Fail('usage', '--select takes all, artist:NAME, album:ARTIST::TITLE or genre:NAME');
      }
      l.selection = s;
    }
  }

  Object.assign(commands, {
    async 'link-ls'() {
      const dev = devId();
      const lf = loadLinks(dev);
      for (const l of lf.links) emit('entry', l);
      return { device_id: dev, client_id: lf.client_id, count: lf.links.length, links: lf.links };
    },

    async 'link-add'(args) {
      const kind = opts.kind;
      if (!KINDS.includes(kind)) throw new Fail('usage', 'link-add needs --kind music|audiobooks|ebooks|video');
      if (args.length !== 1) throw new Fail('usage', 'link-add needs --kind KIND LOCAL_FOLDER');
      const dir = args[0];
      if (!existsSync(dir)) throw new Fail('local_io', `${dir} is not a folder`, undefined, { path: dir });
      const dev = devId();
      const lf = loadLinks(dev);
      if (lf.links.some((l) => l.kind === kind && fold(l.local.path) === fold(dir))) throw new Fail('usage', `${dir} is already linked for ${kind}`);
      const label = opts.label || path.basename(dir) || kind;
      let root = ROOTS[kind];
      if (!opts['card-root'] && lf.links.some((l) => fold(l.card_root) === fold(root))) root = `${root}/${clean(label)}`;
      const l = {
        id: hex(16), kind, local: { type: 'path', path: dir }, root_marker: { volume_id: 'ntfs:1A2B3C4D', entries_last: 0 },
        card_root: root, layout: kind === 'music' ? 'artist-album' : 'keep',
        selection: { mode: 'all', artists: [], albums: [], genres: [], files: [] }, remove_unselected: false,
        player_only: SCENARIO === 'ask_policy' ? 'ask' : 'keep', include_sidecars: true, exclude: [], enabled: true, label, covers: 'optimize',
      };
      applyOptions(l);
      lf.links.push(l);
      saveLinks(dev, lf);
      return { device_id: dev, link: l };
    },

    async 'link-set'(args) {
      const dev = devId();
      const lf = loadLinks(dev);
      const l = lf.links.find((x) => x.id === args[0]);
      if (!l) throw new Fail('usage', `no link ${args[0]} (link-ls lists them)`);
      if (opts.local) {
        if (!existsSync(opts.local)) throw new Fail('local_io', `${opts.local} is not a folder`);
        l.local = { type: 'path', path: opts.local };
      }
      applyOptions(l);
      saveLinks(dev, lf);
      return { device_id: dev, link: l };
    },

    async 'link-rm'(args) {
      const dev = devId();
      const lf = loadLinks(dev);
      const i = lf.links.findIndex((x) => x.id === args[0]);
      if (i < 0) throw new Fail('usage', `no link ${args[0]} (link-ls lists them)`);
      const [gone] = lf.links.splice(i, 1);
      saveLinks(dev, lf);
      return { device_id: dev, removed: gone.id, count: lf.links.length };
    },

    async 'sync-scan'() {
      const dev = devId();
      const kinds = kindList();
      const links = loadLinks(dev).links.filter((l) => kinds.includes(l.kind) && l.enabled);
      if (!links.length) throw new Fail('usage', `no enabled link for ${kinds.join(', ')}; add one with link-add`);
      const out = [];
      for (const l of links) out.push({ id: l.id, kind: l.kind, ...(await scanLink(l)) });
      return { device_id: dev, links: out };
    },

    async 'sync-plan'() {
      const kinds = kindList();
      connect();
      requireSd();
      const dev = DEVICE_ID;
      const lf = loadLinks(dev);
      const links = lf.links.filter((l) => kinds.includes(l.kind) && l.enabled);
      if (!links.length) throw new Fail('usage', `no enabled link for ${kinds.join(', ')}; add one with link-add`);
      const state = load();
      const ss = syncState(state);
      const scans = {};
      for (const l of links) scans[l.id] = await scanLink(l);
      const items = [];
      const summary = {};
      const linksOut = [];
      let n = 0;
      const id = () => `i${++n}`;
      for (const l of links) {
        emit('progress', { op: 'list', path: l.card_root, done: 0, total: 0, rate: 0, unit: 'folders' });
        await tick(STEP * 4);
        const scan = scans[l.id];
        const r = linkItems(l, scan, ss, id, !!opts['delete-untracked']);
        items.push(...r.items);
        linksOut.push({ id: l.id, kind: l.kind, label: l.label, local: l.local.path, card_root: l.card_root, layout: l.layout,
          scan: { status: scan.status, time: Math.floor(Date.now() / 1000), errors: 0, unreadable: 0, tag_errors: 0 }, guard: r.guard, tracked: r.tracked });
        if (r.guard.tripped) {
          emit('warn', r.guard.reason === 'mass_delete'
            ? { code: 'mass_delete_blocked', message: 'Many files would be removed from the HiBy; none is ticked. Check that the folder is complete.', link: l.id, reason: r.guard.reason }
            : { code: 'source_unavailable', message: `Your ${l.kind} folder looks unavailable (${r.guard.reason}). Nothing will be deleted.`, link: l.id, reason: r.guard.reason });
        }
      }
      const df = dfFields(state);
      const sdFree = SCENARIO === 'card_full' ? 900000000 : df.sd_free;
      for (const k of kinds) {
        const ki = items.filter((i) => i.kind === k && !i.parent);
        if (!links.some((l) => l.kind === k)) continue;
        const count = (a) => ki.filter((i) => i.action === a).length;
        const po = {};
        for (const i of ki.filter((x) => x.action === 'player_only')) po[i.reason] = (po[i.reason] ?? 0) + 1;
        const g = linksOut.find((x) => x.kind === k && x.guard.tripped)?.guard ?? { tripped: false, reason: null };
        const bytesAdd = ki.filter((i) => i.action === 'add').reduce((s, i) => s + i.size, 0) +
          ki.filter((i) => i.action === 'update').reduce((s, i) => s + Math.max(0, i.size - i.card_size), 0);
        const bytesRemove = ki.filter((i) => i.action === 'player_only' && i.default_choice === 'delete').reduce((s, i) => s + i.size, 0);
        summary[k] = {
          add: count('add'), update: count('update'), move: count('move'), adopt: count('adopt'), record: count('record'),
          skip: linksOut.filter((x) => x.kind === k).reduce((s, x) => s + x.tracked, 0), conflict: count('conflict'), player_only: po,
          guard: g, blocked: count('blocked'), excluded: 0, unreadable_kept: 0, needs_decision: ki.filter((i) => i.needs_decision).length,
          bytes_add: bytesAdd, bytes_remove: bytesRemove, bytes_fingerprint: 0, sd_free: sdFree,
          fits: bytesAdd - bytesRemove + 64 * 1048576 <= sdFree, eta_s: Math.round(bytesAdd / 11200000) + Math.round(ki.length / 20),
        };
      }
      const add = Object.values(summary).reduce((s, x) => s + x.bytes_add - x.bytes_remove, 0);
      const plan = {
        format: 2, device_id: dev, card_id: 'card0123456789ab', client_id: lf.client_id, run_id: `${new Date().toISOString().replace(/[-:]/g, '').slice(0, 15)}-${hex(8)}`,
        created: Math.floor(Date.now() / 1000), kinds: Object.keys(summary), fs: 'exfat', sd_free: sdFree,
        fits: add + 64 * 1048576 <= sdFree, summary, links: linksOut, items,
      };
      const f = opts.out || cachePath('sync', 'plan-v2.json');
      writeFileSync(`${f}.tmp`, JSON.stringify(plan));
      renameSync(`${f}.tmp`, f);
      return { plan: f, run_id: plan.run_id, kinds: plan.kinds, fits: plan.fits, sd_free: sdFree, items: items.length, summary, size: JSON.stringify(plan).length };
    },

    async 'sync-apply'(args) {
      if (args.length !== 1) throw new Fail('usage', 'sync-apply needs PLAN.json');
      let plan;
      try { plan = JSON.parse(readFileSync(args[0], 'utf8')); } catch { throw new Fail('local_io', `${args[0]} cannot be read`); }
      let dec = {};
      if (opts.decisions) { try { dec = JSON.parse(readFileSync(opts.decisions, 'utf8')); } catch { throw new Fail('local_io', `${opts.decisions} cannot be read`); } }
      const choices = resolve(plan.items, dec);
      const guarded = plan.items.filter((i) => !i.parent && choices[i.id] === 'delete' && i.guard === true);
      if (guarded.length) {
        let ok;
        if (opts['allow-mass-delete'] && opts.yes) ok = true;
        else if (opts.yes) ok = false;
        else {
          emit('prompt', { kind: 'confirm_mass_delete', message: `Type ${guarded.length} to delete ${guarded.length} files from the HiBy although the folder looks incomplete`, count: guarded.length, bytes: guarded.reduce((s, i) => s + i.size, 0) });
          const a = await nextAnswer();
          ok = String(a ?? '').trim() === String(guarded.length);
        }
        if (!ok) {
          emit('warn', { code: 'mass_delete_blocked', message: `${guarded.length} deletions were not confirmed; those files stay on the HiBy`, count: guarded.length });
          for (const g of guarded) choices[g.id] = 'keep';
        }
      }
      const deletes = plan.items.filter((i) => !i.parent && i.action === 'player_only' && choices[i.id] === 'delete');
      if (deletes.length && !opts.yes) {
        emit('prompt', { kind: 'confirm', message: `Delete ${deletes.length} files from the HiBy?`, count: deletes.length, bytes: deletes.reduce((s, i) => s + i.size, 0) });
        const a = await nextAnswer();
        if (a !== true && a !== 'yes' && a !== 'y') throw new Fail('cancelled', 'nothing was copied or deleted');
      }
      connect();
      return runApply(plan, choices, null);
    },

    async 'sync-resume'() {
      connect();
      const state = load();
      const run = syncState(state).runs.at(-1);
      if (!run) return { resumed: false, status: 'nothing_to_resume' };
      emit('warn', { code: 'resumed', message: `Going on with the sync of ${run.kinds.join(', ')} (${run.done} files were already copied)`, run_id: run.run_id });
      const r = await runApply(run.plan, run.choices, run);
      return { ...r, resumed: true };
    },

    async 'sync-status'() {
      const dev = devId();
      const state = load();
      const ss = syncState(state);
      const lf = loadLinks(dev);
      return {
        device_id: dev, client_id: lf.client_id,
        links: lf.links.map((l) => ({ id: l.id, kind: l.kind, label: l.label, local: l.local.path, card_root: l.card_root, enabled: l.enabled,
          tracked: ss.applied[l.id] ? trackedCount(l) : 0, last_sync: ss.lastSync[l.id] ?? null, scanned: Math.floor(Date.now() / 1000) - 60 })),
        runs: ss.runs.map((r) => ({ run_id: r.run_id, kinds: r.kinds, started: r.started, done: r.done, status: r.status, resumable: true })),
        card_id: 'card0123456789ab',
        lock: SCENARIO === 'sync_locked' && !ss.tookOver ? lockInfo() : { held: false },
        card_journals: ss.runs.length,
      };
    },

    async 'sync-unlock'() {
      connect();
      const state = load();
      const ss = syncState(state);
      if (SCENARIO !== 'sync_locked' || ss.tookOver) return { held: false, unlocked: false };
      if (!opts.force) throw new Fail('locked', 'The HiBy is being synced from ANA-LAPTOP; add --force to remove its lock', undefined, { locked_by: lockInfo() });
      ss.tookOver = true;
      save(state);
      return { held: true, unlocked: true, ours: false, stale: false, locked_by: lockInfo() };
    },
  });

  function lockInfo() {
    return { held: true, ours: false, stale: false, client_id: 'c-ana-laptop', name: 'ANA-LAPTOP', heartbeat: Math.floor(Date.now() / 1000) - 140, run_id: '20261112T101500-77aa' };
  }

  function kindList() {
    const k = opts.kind ?? 'all';
    if (k === 'all') return [...KINDS];
    if (!KINDS.includes(k)) throw new Fail('usage', '--kind is music, audiobooks, ebooks, video or all');
    return [k];
  }

  function unavailable(l) {
    return (SCENARIO === 'sync_unavailable' && l.kind === 'music') || !existsSync(l.local.path);
  }

  async function scanLink(l) {
    if (unavailable(l)) {
      emit('warn', { code: 'source_unavailable', message: `${l.local.path} is not available (missing). Nothing will be deleted.`, link: l.id, status: 'missing' });
      return { status: 'missing', files: 0, unreadable: 0, errors: 0, tag_errors: 0, bytes: 0 };
    }
    const files = sourceFiles(l);
    for (let i = 1; i <= 4; i++) {
      await tick(STEP);
      emit('progress', { op: 'scan', path: `${l.local.path}/${i}`, done: i * 6, total: 0, rate: 0, unit: 'folders' });
    }
    if (l.kind === 'music' || l.kind === 'audiobooks') {
      for (let i = 0; i <= files.length; i += Math.max(1, Math.floor(files.length / 6))) {
        await tick(STEP);
        emit('progress', { op: 'tags', path: files[Math.min(i, files.length - 1)]?.rel ?? null, done: i, total: files.length, rate: 0, unit: 'files' });
      }
    }
    return { status: 'ok', files: files.length, unreadable: 0, errors: 0, tag_errors: 0, bytes: files.reduce((s, f) => s + f.size, 0) };
  }

  // The made-up files of a linked folder: { rel, dest, size, title, artist, album, genre, track }.
  function sourceFiles(l) {
    const root = l.card_root;
    if (l.kind === 'music') {
      const cat = localCatalog([l.local.path.replace(/\\/g, '/')]);
      const sel = l.selection ?? { mode: 'all' };
      const all = sel.mode !== 'chosen';
      const artists = new Set((sel.artists ?? []).map(fold));
      const genres = new Set((sel.genres ?? []).map(fold));
      const albums = new Set((sel.albums ?? []).map((a) => `${fold(a.artist)}\u0000${fold(a.title)}`));
      return cat.tracks.filter((t) => !t.book).map((t) => ({
        rel: `${clean(t.artist)}/${clean(t.album)}/${pad(t.track)} ${clean(t.title)}.${t.format.toLowerCase()}`,
        dest: l.layout === 'keep'
          ? `${root}/${clean(t.artist)}/${clean(t.album)}/${pad(t.track)} ${clean(t.title)}.${t.format.toLowerCase()}`
          : `${root}/${clean(t.artist)}/${clean(t.album)}/${pad(t.track)} ${clean(t.title)}.${t.format.toLowerCase()}`,
        size: t.size, title: t.title, artist: t.artist, album: t.album, genre: t.genre, track: t.track,
        selected: all || artists.has(fold(t.artist)) || genres.has(fold(t.genre)) || albums.has(`${fold(t.artist)}\u0000${fold(t.album)}`),
      }));
    }
    if (l.kind === 'audiobooks') {
      const cat = localCatalog([l.local.path.replace(/\\/g, '/')]);
      return cat.tracks.filter((t) => t.book).map((t) => ({
        rel: `${clean(t.album)}/${pad(t.track)} ${clean(t.title)}.mp3`, dest: `${root}/${clean(t.album)}/${pad(t.track)} ${clean(t.title)}.mp3`,
        size: t.size, title: `${t.album} — ${t.title}`, selected: true,
      }));
    }
    if (l.kind === 'ebooks') {
      const names = ['Ada Calloway - The Glass Orchard', 'Ines Moreau - Northern Letters', 'Tomas Reyes - A Map of Rain',
        'Priya Nair - The Lantern Year', 'Henrik Sol - Small Weather', 'Mara Quint - Atlas of Tides'];
      return names.map((nm, i) => ({ rel: `${nm}.epub`, dest: `${root}/${nm}.epub`, size: 650000 + i * 91000, title: nm, selected: true }));
    }
    const vids = [['Concert at the Harbour (2024).mp4', 1840000000], ['Making of Blue Hours.mkv', 912000000],
      ['Live in Lisbon — the full night with all the encores, the interviews, the rehearsals and the backstage film (Director’s Cut, 4K remaster).mkv', 3100000000]];
    return vids.map(([nm, size]) => ({ rel: nm, dest: `${root}/${nm}`, size, title: nm.replace(/\.[^.]+$/, ''), selected: true }));
  }

  function trackedCount(l) {
    return sourceFiles(l).filter((f) => f.selected).length;
  }

  // The plan items of one link, per scenario.
  function linkItems(l, scan, ss, id, deleteUntracked) {
    const items = [];
    const applied = !!ss.applied[l.id];
    const policy = l.player_only ?? 'keep';
    let guard = { tripped: false, reason: null };
    const po = (reason, dest, size, title, extra = {}) => {
      const ask = policy === 'ask';
      let def = 'keep';
      if (reason === 'deleted_on_computer' && policy === 'delete') def = 'delete';
      if (reason === 'excluded_on_computer' && l.remove_unselected) def = 'delete';
      if (['from_other_device', 'added_on_player', 'downloaded_on_player'].includes(reason) && deleteUntracked) def = 'delete';
      const it = {
        id: id(), kind: l.kind, link: l.id, action: 'player_only', reason, src_rel: null, dest, old_dest: null, size, mtime: 1767225600,
        card_size: size, card_mtime: 1767225600, title, origin: reason === 'from_other_device' ? 'client:ana-laptop' : reason === 'deleted_on_computer' ? `client:${CLIENT_ID}` : null,
        default_choice: ask && reason === 'deleted_on_computer' ? null : def, choices: ['delete', 'keep', 'pull'], ...extra,
      };
      if (ask && reason === 'deleted_on_computer') it.needs_decision = true;
      items.push(it);
      return it;
    };
    if (scan.status !== 'ok') {
      // the folder is gone: every file this computer synced looks deleted, and none is ticked
      guard = { tripped: true, reason: 'source_unavailable' };
      const lost = sourceFiles({ ...l, local: { path: 'C:/Music' } }).slice(0, 120);
      for (const f of lost) po('deleted_on_computer', f.dest, f.size, f.title, { guard: true });
      for (const it of items) it.default_choice = 'keep';
      return { items, guard, tracked: lost.length };
    }
    const files = sourceFiles(l).filter((f) => f.selected);
    if (applied) return { items, guard, tracked: files.length };
    const add = (f, extra = {}) => {
      const it = { id: id(), kind: l.kind, link: l.id, action: 'add', reason: 'new', src_rel: f.rel, dest: f.dest, old_dest: null, size: f.size,
        mtime: 1714000000, card_size: -1, card_mtime: 0, title: f.title, origin: null, default_choice: null, choices: [], covers: 'optimize', ...extra };
      items.push(it);
      return it;
    };
    let tracked = 0;
    if (l.kind === 'music') {
      const onHiby = new Set(hibyIndex().tracks.map((t) => `${fold(t.artist)}\u0000${fold(t.album)}\u0000${t.track}`));
      let moved = 0;
      let albumMoved = null;
      for (const f of files) {
        const there = onHiby.has(`${fold(f.artist)}\u0000${fold(f.album)}\u0000${f.track}`);
        if (!there) {
          const it = add(f);
          if (f.track === 1 && items.filter((x) => x.action === 'add').length < 40) {
            items.push({ id: id(), kind: l.kind, link: l.id, action: 'add', reason: 'new', src_rel: f.rel.replace(/\.[^.]+$/, '.lrc'), dest: f.dest.replace(/\.[^.]+$/, '.lrc'),
              old_dest: null, size: 2400, mtime: 1714000000, card_size: -1, card_mtime: 0, title: f.title, origin: null, default_choice: null, choices: [], parent: it.id });
          }
          continue;
        }
        tracked++;
        if (f.track === 3) {
          items.push({ id: id(), kind: l.kind, link: l.id, action: 'update', reason: 'changed', src_rel: f.rel, dest: f.dest, old_dest: null, size: f.size,
            mtime: 1714003600, card_size: f.size - 4096, card_mtime: 1767225600, title: f.title, origin: `client:${CLIENT_ID}`, default_choice: null, choices: [] });
        } else if ((albumMoved === null || albumMoved === f.album) && moved < 6 && f.track > 3) {
          albumMoved = f.album;
          moved++;
          const old = f.dest.replace(`/${clean(f.album)}/`, `/${clean(f.album.toLowerCase())} (Remastered)/`);
          items.push({ id: id(), kind: l.kind, link: l.id, action: 'move', reason: 'dest_changed', src_rel: f.rel, dest: f.dest, old_dest: old, size: f.size,
            mtime: 1714000000, card_size: f.size, card_mtime: 1767225600, title: f.title, origin: `client:${CLIENT_ID}`, default_choice: null, choices: [] });
        }
      }
      // a few files the HiBy already has at the right place: recorded, not copied
      for (let k = 0; k < 6; k++) {
        items.push({ id: id(), kind: l.kind, link: l.id, action: 'adopt', reason: 'same_content', src_rel: `x/${k}.flac`, dest: `${l.card_root}/x/${k}.flac`, old_dest: null,
          size: 30000000, mtime: 1, card_size: 30000000, card_mtime: 1, title: `x${k}`, origin: null, default_choice: null, choices: [] });
      }
      if (SCENARIO === 'sync_mass_delete') {
        guard = { tripped: true, reason: 'mass_delete' };
        for (let k = 1; k <= 240; k++) po('deleted_on_computer', `${l.card_root}/Old Collection/Disc ${Math.ceil(k / 12)}/${pad(((k - 1) % 12) + 1)} Track ${k}.flac`, 28000000 + k * 1000, `Track ${k}`, { guard: true });
        for (const it of items) if (it.action === 'player_only') it.default_choice = 'keep';
      } else {
        po('deleted_on_computer', `${l.card_root}/The Midnight Static/Neon Tides/04 Paper boats.flac`, 31400000, 'Paper boats');
        po('deleted_on_computer', `${l.card_root}/The Midnight Static/Neon Tides/05 Gravity.flac`, 28700000, 'Gravity');
        const lrc = { id: id(), kind: l.kind, link: l.id, action: 'player_only', reason: 'deleted_on_computer', src_rel: null,
          dest: `${l.card_root}/The Midnight Static/Neon Tides/05 Gravity.lrc`, old_dest: null, size: 2100, mtime: 1, card_size: 2100, card_mtime: 1,
          title: 'Gravity', origin: null, default_choice: policy === 'delete' ? 'delete' : 'keep', choices: ['delete', 'keep', 'pull'], parent: items.at(-1).id };
        items.push(lrc);
        po('deleted_on_computer', `${l.card_root}/Glass Harbour/Static Garden/01 Halo.flac`, 33100000, 'Halo');
      }
      if (l.selection?.mode === 'chosen') {
        const out = sourceFiles(l).filter((f) => !f.selected).slice(0, 5);
        for (const f of out) po('excluded_on_computer', f.dest, f.size, f.title);
      }
      po('from_other_device', `${l.card_root}/Elena Okafor/Velvet Hours/02 Closer.flac`, 30800000, 'Closer');
      po('added_on_player', `${l.card_root}/ABBA/Arrival/01 When I Kissed the Teacher.flac`, 25165824, 'When I Kissed the Teacher');
      po('added_on_player', `${l.card_root}/ABBA/Arrival/02 Dancing Queen.flac`, 29360128, 'Dancing Queen');
      po('added_on_player', `${l.card_root}/Daft Punk/Discovery/01 One More Time.flac`, 41943040, 'One More Time');
      po('added_on_player', `${l.card_root}/Daft Punk/Discovery/02 Aerodynamic.flac`, 26214400, 'Aerodynamic');
      po('added_on_player', `${l.card_root}/Ñandú/Canción (live).mp3`, 8388608, 'Canción (live)');
      const c = files.find((f) => onHiby.has(`${fold(f.artist)}\u0000${fold(f.album)}\u0000${f.track}`) && f.track === 2);
      if (c) {
        items.push({ id: id(), kind: l.kind, link: l.id, action: 'conflict', reason: 'edited_on_player', src_rel: c.rel, dest: c.dest, old_dest: null, size: c.size,
          mtime: 1714000000, card_size: c.size + 8192, card_mtime: 1767300000, title: c.title, origin: `client:${CLIENT_ID}`, default_choice: 'skip',
          choices: ['computer', 'player', 'both', 'skip'] });
      }
    } else if (l.kind === 'audiobooks') {
      for (const f of files) add(f);
      po('downloaded_on_player', `${l.card_root}/Audio Book - Part 1.m4b`, 104857600, 'Audio Book - Part 1');
    } else if (l.kind === 'ebooks') {
      for (const f of files) add(f);
      po('downloaded_on_player', `${l.card_root}/Jane Austen - Persuasion.epub`, 480000, 'Jane Austen - Persuasion');
      po('downloaded_on_player', `${l.card_root}/Herman Melville - Moby Dick.epub`, 1300000, 'Herman Melville - Moby Dick');
    } else {
      for (const f of files) {
        if (f.dest.length > 120) {
          items.push({ id: id(), kind: l.kind, link: l.id, action: 'blocked', reason: 'bad_path_too_long', src_rel: f.rel, dest: f.dest, old_dest: null, size: f.size,
            mtime: 1714000000, card_size: -1, card_mtime: 0, title: f.title, origin: null, default_choice: null, choices: [] });
        } else add(f);
      }
    }
    return { items, guard, tracked };
  }

  // Decisions.resolve: the user's choices, else the pre-ticked ones; sidecars follow their parent.
  function resolve(items, dec) {
    const out = {};
    const byId = new Map(items.map((i) => [i.id, i]));
    for (const [k, v] of Object.entries(dec)) {
      if (k === 'policy') continue;
      const it = byId.get(k);
      if (!it) throw new Fail('usage', `DEC.json names ${k}, which is not an item of this plan`);
      if (it.parent) throw new Fail('usage', `${k} follows its track or folder; decide on ${it.parent} instead`);
      if (!it.choices.includes(v)) throw new Fail('usage', `${k}: "${v}" is not one of ${it.choices.join(', ')}`);
      out[k] = v;
    }
    for (const it of items) {
      if (it.parent || Object.hasOwn(out, it.id)) continue;
      if (it.action === 'player_only') out[it.id] = it.default_choice ?? 'keep';
      else if (it.action === 'conflict') out[it.id] = it.default_choice ?? 'skip';
    }
    for (const it of items) if (it.parent) out[it.id] = out[it.parent] ?? 'keep';
    return out;
  }

  async function runApply(plan, choices, resumeRun) {
    const state = load();
    const ss = syncState(state);
    if (SCENARIO === 'sync_locked' && !opts['take-over'] && !ss.tookOver) {
      throw new Fail('locked', 'The HiBy is being synced from ANA-LAPTOP (last seen 2 min ago)', undefined, { locked_by: lockInfo() });
    }
    if (SCENARIO === 'sync_locked') { ss.tookOver = true; save(state); emit('warn', { code: 'lock_taken_over', message: 'The sync lock of ANA-LAPTOP was taken over' }); }
    if (SCENARIO === 'sync_battery') {
      throw new Fail('remote', "The HiBy's battery is at 11 %. Charge it, then sync again.", { code: 67, name: 'E_BATTERY' }, { battery: 11, charging: false });
    }
    requireSd();
    const items = plan.items;
    const kinds = {};
    for (const kd of plan.kinds) kinds[kd] = { written: 0, skipped: 0, moved: 0, removed: 0, pulled: 0, recorded: 0, failed: 0, not_fitted: 0, blocked: 0, conflicts_left: 0, kept: 0, bytes: 0 };
    const kr = (it) => kinds[it.kind] ?? (kinds[it.kind] = { written: 0, skipped: 0, moved: 0, removed: 0, pulled: 0, recorded: 0, failed: 0, not_fitted: 0, blocked: 0, conflicts_left: 0, kept: 0, bytes: 0 });
    const failures = [];
    const top = items.filter((i) => !i.parent);
    const puts = top.filter((i) => i.action === 'add' || i.action === 'update' || (i.action === 'conflict' && ['computer', 'both'].includes(choices[i.id])));
    const run = resumeRun ?? { run_id: plan.run_id, kinds: plan.kinds, started: Math.floor(Date.now() / 1000), done: 0, status: 'interrupted', plan, choices };
    const record = (status) => {
      const st = load();
      const s2 = syncState(st);
      run.status = status;
      s2.runs = s2.runs.filter((r) => r.run_id !== run.run_id);
      s2.runs.push(run);
      if (s2.tookOver === undefined && ss.tookOver) s2.tookOver = ss.tookOver;
      save(st);
    };
    if (!resumeRun) {
      for (const [i, it] of top.filter((x) => x.action === 'move').entries()) {
        await tick(STEP);
        emit('progress', { op: 'move', path: it.dest, done: i + 1, total: 0, rate: 0, unit: 'files' });
        kr(it).moved++;
      }
      const dels = top.filter((x) => x.action === 'player_only' && choices[x.id] === 'delete');
      for (const [i, it] of dels.entries()) {
        await tick(STEP);
        emit('progress', { op: 'remove', path: it.dest, done: i + 1, total: dels.length, rate: 0, unit: 'files' });
        kr(it).removed++;
      }
      for (const it of top.filter((x) => x.action === 'player_only' && choices[x.id] === 'keep')) kr(it).kept++;
      for (const it of top.filter((x) => x.action === 'adopt' || x.action === 'record')) kr(it).recorded++;
      for (const it of top.filter((x) => x.action === 'blocked')) kr(it).blocked++;
      for (const it of top.filter((x) => x.action === 'conflict' && choices[x.id] === 'skip')) kr(it).conflicts_left++;
    }
    const total = puts.reduce((s, i) => s + i.size, 0);
    let doneBytes = puts.slice(0, run.done).reduce((s, i) => s + i.size, 0);
    const steps = SCENARIO === 'slow' ? 5 : 2;
    const failing = SCENARIO === 'sync_partial' ? new Set([3, 7, 12].map((x) => puts[x]?.id).filter(Boolean)) : new Set();
    const stopAt = !resumeRun && ['sync_pause', 'sync_usb_pause'].includes(SCENARIO) ? Math.floor(puts.length * 0.4) : -1;
    const rate = SCENARIO === 'slow' ? 2400000 : 11200000;
    try {
      for (let x = run.done; x < puts.length; x++) {
        const it = puts[x];
        if (x === stopAt) {
          emit('warn', { code: 'reconnecting', message: 'connection lost; retrying in 2.0 s', path: it.dest });
          await tick(STEP * 3);
          const status = SCENARIO === 'sync_usb_pause' ? 'paused_usb_storage' : 'paused';
          emit('warn', { code: status, message: status === 'paused' ? 'The HiBy stopped answering; run sync-resume to go on' : "The HiBy's card is connected to the computer as a USB drive; run sync-resume to go on" });
          record(status);
          return { status, run_id: run.run_id, resumable: true, kinds, failures, library: { action: opts.then || 'none', ok: false } };
        }
        emit('progress', { op: 'sync', path: it.dest, done: doneBytes, total, rate, unit: 'bytes', files_done: x, files_total: puts.length });
        if (failing.has(it.id)) {
          await tick(STEP);
          kr(it).failed++;
          failures.push({ id: it.id, dest: it.dest, src_rel: it.src_rel, error: 'E_IO', message: 'The HiBy could not write the file' });
          emit('warn', { code: 'failed', message: `${it.dest}: The HiBy could not write the file`, path: it.dest });
          run.done = x + 1;
          continue;
        }
        if (it.kind === 'music') {
          await tick(STEP / 2);
          emit('progress', { op: 'prepare', path: it.dest, done: 0, total: 0, rate: 0, unit: 'none' });
        }
        for (let i = 1; i <= steps; i++) {
          await tick(STEP);
          emit('progress', { op: 'put', path: it.dest, done: Math.round((it.size * i) / steps), total: it.size, rate, unit: 'bytes' });
        }
        doneBytes += it.size;
        kr(it).written++;
        kr(it).bytes += it.size;
        run.done = x + 1;
      }
    } catch (e) {
      if (isCancelled() || e?.code === 'cancelled') {
        record('interrupted');
        throw new Fail('cancelled', 'cancelled; run sync-resume to go on', undefined, { run_id: run.run_id, resumable: true });
      }
      throw e;
    }
    emit('progress', { op: 'sync', path: null, done: total, total, rate, unit: 'bytes', files_done: puts.length, files_total: puts.length });
    const pulls = top.filter((x) => x.action === 'player_only' && choices[x.id] === 'pull');
    for (const [i, it] of pulls.entries()) {
      await tick(STEP);
      emit('progress', { op: 'pull', path: it.dest, done: it.size, total: it.size, rate, unit: 'bytes', index: i });
      kr(it).pulled++;
    }
    const then = opts.then || 'none';
    let library = { action: then, ok: then === 'none' };
    if (then !== 'none') {
      emit('progress', { op: 'library', path: null, done: 0, total: 0, rate: 0, unit: 'none' });
      await tick(STEP * 30);
      if (SCENARIO === 'sync_db_pending') {
        emit('warn', { code: 'library_pending', message: 'Music copied. To see it in the player, open Settings > Update library on the HiBy.' });
        library = { action: then, ok: false, code: 'E_UNSUPPORTED' };
      } else library = { action: then, ok: true, count: Object.values(kinds).reduce((s, x) => s + x.written, 0) };
    }
    const st = load();
    const s2 = syncState(st);
    s2.runs = s2.runs.filter((r) => r.run_id !== run.run_id);
    for (const l of plan.links ?? []) { s2.applied[l.id] = true; s2.lastSync[l.id] = Math.floor(Date.now() / 1000); }
    if (ss.tookOver) s2.tookOver = true;
    save(st);
    const status = failures.length ? 'done_with_problems' : 'done';
    if (failures.length) emit('warn', { code: 'failed', message: `${failures.length} files were not copied`, count: failures.length });
    return { status, run_id: run.run_id, resumable: false, kinds, failures, library };
  }
}
