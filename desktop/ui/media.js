// The libraries (docs/DESKTOP_APP.md, SYNC_LIBRARY_DESIGN.md 10.1): the HiBy's library in the
// device's Library tab, and the computer's library (the linked folders, read by music-scan) in
// the sidebar, both as iTunes 11 showed one (albums grid that opens in place, artists, songs,
// audiobooks, playlists). One set of views draws either; the panel moves between the two pages.
// Every device operation is a byos-cli job; names from the HiBy and from music files only ever
// go into the page with textContent. The Sync tab is ui/sync.js.

import {
  age, compareNames, fmtDuration, groupIndex, libraryState, matches, quality, scanEstimate, textOn, virtualWindow,
} from '/media-format.js';

let C; // the app's helpers (initMedia)
const ROW_H = 28;

const newLib = (local) => ({
  deviceId: null, index: null, state: 'idle', progress: null, error: null, view: 'albums', query: '', open: null,
  artist: 0, playlist: null, playlists: null, sort: { key: 'title', dir: 1 }, selected: new Set(), lastClicked: null,
  stale: false, stalePaths: [], busy: null, art: new Map(), artQueue: new Set(), artTimer: null, artRunning: false,
  loading: false, demo: null, local,
});
// The HiBy's library (device Library tab) and the computer's (sidebar). "lib" is the one on screen.
const devLib = newLib(false);
const pcLib = newLib(true);
let lib = devLib;

const $ = (id) => document.getElementById(id);
const t = (...a) => C.t(...a);
const el = (...a) => C.el(...a);
const lang = () => C.getLang();
const fmtBytes = (n) => C.fmtBytes(n);
const fmtCount = (n) => new Intl.NumberFormat(lang()).format(n ?? 0);
const device = () => C.selectedDevice();
const spec = () => C.deviceSpec(device());

function totalTime(ms) {
  const m = Math.round((ms ?? 0) / 60000);
  return m >= 60 ? t('totalHours', { h: Math.floor(m / 60), m: m % 60 }) : t('totalMin', { m });
}

function storage(key, value) {
  try {
    if (value === undefined) return JSON.parse(localStorage.getItem(key) ?? 'null');
    localStorage.setItem(key, JSON.stringify(value));
  } catch { /* storage may be blocked */ }
  return null;
}

export function toast(text, kind = '') {
  const n = $('toast');
  n.textContent = text;
  n.className = `toast ${kind}`;
  n.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { n.hidden = true; }, 3800);
}

const SVG_NS = 'http://www.w3.org/2000/svg';
export function icon(d, cls = 'i') {
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', cls);
  for (const part of [].concat(d)) {
    const p = document.createElementNS(SVG_NS, 'path');
    p.setAttribute('d', part);
    p.setAttribute('fill', 'none');
    p.setAttribute('stroke', 'currentColor');
    p.setAttribute('stroke-width', '1.7');
    p.setAttribute('stroke-linecap', 'round');
    p.setAttribute('stroke-linejoin', 'round');
    svg.append(p);
  }
  return svg;
}
export const ICONS = {
  note: ['M9 18V5l11-2v13', 'M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0Z', 'M20 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0Z'],
  folder: ['M3 7.5A1.5 1.5 0 0 1 4.5 6H9l2 2h8.5A1.5 1.5 0 0 1 21 9.5v8A1.5 1.5 0 0 1 19.5 19h-15A1.5 1.5 0 0 1 3 17.5Z'],
  check: ['M5 12.5 10 17.5 19 7'],
  warn: ['M12 4 21 19H3Z', 'M12 10v4', 'M12 16.5v.5'],
  info: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z', 'M12 11v6', 'M12 7.5v.5'],
  close: ['M7 7l10 10M17 7 7 17'],
  db: ['M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3Z', 'M4 6v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6', 'M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3'],
  sd: ['M7 3h8l4 4v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1Z', 'M10 3v4M13 3v4M16 4v3'],
  sync: ['M20 12a8 8 0 0 1-13.7 5.7', 'M4 12a8 8 0 0 1 13.7-5.7', 'M18 3v4h-4', 'M6 21v-4h4'],
  headphones: ['M4 15v-3a8 8 0 0 1 16 0v3', 'M4 15a2 2 0 0 1 2-2h1v7H6a2 2 0 0 1-2-2Z', 'M20 15a2 2 0 0 0-2-2h-1v7h1a2 2 0 0 0 2-2Z'],
  book: ['M4 5.5A1.5 1.5 0 0 1 5.5 4H11v16H5.5A1.5 1.5 0 0 1 4 18.5Z', 'M20 5.5A1.5 1.5 0 0 0 18.5 4H13v16h5.5a1.5 1.5 0 0 0 1.5-1.5Z'],
  film: ['M4 5h16v14H4Z', 'M8 5v14M16 5v14', 'M4 9h4M4 15h4M16 9h4M16 15h4'],
  plus: ['M12 5v14M5 12h14'],
  refresh: ['M19 12a7 7 0 1 1-2.05-4.95', 'M19 4v4h-4'],
  move: ['M5 12h13', 'M14 7l5 5-5 5'],
  block: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z', 'M5.6 5.6l12.8 12.8'],
  pause: ['M9 5v14M15 5v14'],
  lock: ['M6 11h12v9H6Z', 'M8.5 11V8a3.5 3.5 0 0 1 7 0v3'],
  battery: ['M3 8h15v8H3Z', 'M21 11v2', 'M6 11h3'],
};

// "2 min ago" for a time in seconds.
export function ageText(sec) { return age(sec, Date.now() / 1000, t); }

// ---- album art: lazy, batched lib-art jobs ------------------------------------------------------

// The album page's cover is 210 CSS px and grid tiles grow past 148: at 125 % display scaling or
// more a 256 px picture would be stretched (blurry), so those screens get the 500 px one.
const ART_SIZE = (globalThis.devicePixelRatio || 1) > 1.1 ? 500 : 256;

let artObserver = null;

function artObserve(node) {
  if (!artObserver) {
    artObserver = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (!e.isIntersecting) continue;
        artObserver.unobserve(e.target);
        requestArt(e.target.dataset.art);
      }
    }, { rootMargin: '300px' });
  }
  artObserver.observe(node);
}

// A cover tile for a card path: the picture once lib-art made it, a tinted note until then.
function cover(path, cls = 'cover') {
  const box = el('div', { class: `${cls} art`, 'data-art': path });
  box.append(icon(ICONS.note, 'art-ph'));
  // the computer's files have no cover service yet: a colour of the album's own
  if (lib.local) { box.classList.add('local'); box.style.setProperty('--ph', phColor(path)); return box; }
  paintArt(box);
  if (!devLib.art.has(path) && device()?.reachable !== false) artObserve(box);
  return box;
}

// A steady colour per album folder, for the computer's covers.
function phColor(path) {
  const dir = String(path ?? '').replace(/[\\/][^\\/]*$/, '');
  let h = 0;
  for (let i = 0; i < dir.length; i++) h = (h * 31 + dir.charCodeAt(i)) >>> 0;
  return `hsl(${h % 360} 46% 52%)`;
}

function paintArt(box) {
  const a = devLib.art.get(box.dataset.art);
  if (!a || a.none) return;
  if (box.querySelector('img')) return;
  const img = el('img', { alt: '', draggable: 'false' });
  img.addEventListener('load', () => box.classList.add('loaded'), { once: true });
  img.addEventListener('error', () => img.remove(), { once: true });
  img.src = `/api/art/${a.key}`;
  box.style.setProperty('--art', a.color ?? '');
  box.append(img);
  if (img.complete && img.naturalWidth) box.classList.add('loaded'); // already in the browser's cache
}

// The covers of a view's first screens in one lib-art job (the rest load as they scroll in).
function prefetchArt(paths) {
  for (const p of paths.slice(0, 48)) requestArt(p);
}

function requestArt(path) {
  if (!path || lib.local || devLib.art.has(path)) return;
  devLib.artQueue.add(path);
  clearTimeout(devLib.artTimer);
  devLib.artTimer = setTimeout(flushArt, 40);
}

async function flushArt() {
  const A = devLib;
  if (A.artRunning || !A.artQueue.size || !device()) return;
  const paths = [...A.artQueue].slice(0, 64);
  for (const p of paths) A.artQueue.delete(p);
  A.artRunning = true;
  const final = await C.runCli('lib-art', { device: spec(), paths, size: ART_SIZE }, { quiet: true });
  A.artRunning = false;
  if (final.ok) {
    for (const it of final.items ?? []) A.art.set(it.path, it.none ? { none: true } : { key: it.key, color: it.color });
    for (const box of document.querySelectorAll('.art[data-art]')) paintArt(box);
    const open = document.querySelector('.album-detail');
    if (open) tintDetail(open);
  } else {
    for (const p of paths) A.art.set(p, { none: true });
  }
  if (A.artQueue.size) flushArt();
}

// ---- library loading -----------------------------------------------------------------------------

async function fetchIndex(id) {
  try { return await C.api(`/api/library/${id}`); } catch { return null; }
}

async function loadLibrary({ force = false, scan = false } = {}) {
  if (lib.local) return loadComputer({ rescan: force });
  const d = device();
  if (!d) return;
  if (d.reachable === false) { lib.state = 'offline'; renderLibrary(); return; }
  if (lib.loading) return;
  if (lib.deviceId !== d.id) {
    Object.assign(lib, { deviceId: d.id, index: null, open: null, selected: new Set(), playlists: null, stale: false, art: new Map() });
  }
  if (!force && !lib.index) {
    const cached = await fetchIndex(d.id);
    if (cached?.tracks) {
      setIndex(cached);
      // a database read is cheap: refreshed in the background; a scan only when asked
      if (cached.source === 'scan' || Date.now() / 1000 - cached.time < 600) return;
    }
  }
  if (!force && lib.index && !scan) return;
  lib.loading = true;
  const first = !lib.index;
  if (first) { lib.state = 'loading'; lib.progress = null; renderLibrary(); }
  const useScan = scan || lib.index?.source === 'scan';
  const final = await C.runCli(useScan ? 'lib-scan' : 'lib-index', { device: spec() }, {
    title: t(useScan ? 'libScanInstead' : 'libLoading'),
    quiet: !first && !useScan,
    onEvent: (ev) => {
      if (ev.event === 'progress' && lib.state === 'loading') { lib.progress = ev; renderLoading(); }
    },
  });
  lib.loading = false;
  if (lib.deviceId !== d.id) return;
  if (final.ok) {
    const idx = await fetchIndex(d.id);
    if (idx) setIndex(idx);
    else { lib.state = 'error'; lib.error = { code: 'internal', message: 'index.json is missing' }; }
  } else if (first || final.hint === 'scan') {
    lib.state = libraryState(final, null);
    lib.error = final;
  }
  renderLibrary();
}

function setIndex(idx, L = lib) {
  // a background refresh that found the same library only updates the footer (no flicker)
  const old = L.index;
  if (old && L.state === 'ready' && idx.revision && old.revision === idx.revision && old.tracks.length === idx.tracks.length) {
    L.index = { ...old, time: idx.time };
    if (L === lib) renderFoot();
    return;
  }
  L.index = idx;
  L.state = libraryState({ ok: true }, idx);
  if (L.artist >= idx.artists.length) L.artist = 0;
  if (L.open !== null && L.open >= idx.albums.length) L.open = null;
  if (L === lib) renderLibrary();
  demoAfter();
}

// The computer's library: the catalogue music-scan wrote for the linked folders; Rebuild reads
// the folders again (only changed files are read).
async function loadComputer({ rescan = false } = {}) {
  const L = pcLib;
  if (L.loading) return;
  if (!rescan && !L.index) {
    try {
      const cat = await C.api('/api/catalog');
      if (cat?.tracks) { setIndex(cat, L); return; }
    } catch { /* not read yet */ }
  }
  if (L.index && !rescan) { if (L === lib) renderLibrary(); return; }
  L.loading = true;
  const first = !L.index;
  if (first) { L.state = 'loading'; L.progress = null; }
  if (L === lib) renderLibrary();
  const final = await C.runCli('music-scan', {}, {
    title: t('pcReading'),
    onEvent: (ev) => { if (ev.event === 'progress' && L.state === 'loading') { L.progress = ev; if (L === lib) renderLoading(); } },
  });
  L.loading = false;
  if (final.ok) {
    try { setIndex(await C.api('/api/catalog'), L); } catch (e) { L.state = 'error'; L.error = { code: 'internal', message: e.message }; }
  } else if (final.code === 'usage' && /folder/.test(final.message ?? '')) {
    L.state = 'nolinks';
  } else if (first) {
    L.state = 'error';
    L.error = final;
  } else toast(C.errorMessage(final), 'fail');
  if (L === lib) renderLibrary();
}

// ---- library rendering ---------------------------------------------------------------------------

function views() {
  for (const b of document.querySelectorAll('#lib-views button')) {
    b.setAttribute('aria-selected', String(b.dataset.view === (lib.view === 'recent' ? 'songs' : lib.view)));
  }
}

function renderLibrary() {
  if (!C || !$('lib-body')) return;
  views();
  for (const b of document.querySelectorAll('.side-lib')) {
    b.classList.toggle('selected', C.appState.view === 'computer' && lib.local && b.dataset.libview === lib.view);
  }
  const ready = lib.state === 'ready';
  $('lib-search').disabled = !ready;
  $('lib-search').placeholder = t('libSearch');
  $('lib-covers').hidden = lib.local;
  $('lib-covers').disabled = !ready || device()?.reachable === false;
  $('lib-refresh').disabled = lib.loading || (!lib.local && device()?.reachable === false);
  $('lib-refresh').title = t(lib.local ? 'pcRebuild' : 'libRefresh');
  const sync = $('lib-sync');
  sync.hidden = !lib.local;
  const kind = lib.view === 'books' ? 'audiobooks' : 'music';
  sync.textContent = t(`sySync_${kind}`);
  sync.disabled = !C.selectedDevice() || C.selectedDevice().reachable === false || lib.state === 'loading';
  sync.title = sync.disabled ? t('pcSyncNeedsHiby') : '';
  sync.onclick = () => C.syncFromComputer(kind);
  renderBanner();
  const body = $('lib-body');
  body.replaceChildren();
  body.className = `lib-body view-${lib.view}`;
  switch (lib.state) {
    case 'loading': body.append(loadingCard()); break;
    case 'offline': body.append(messageCard(ICONS.sd, t('libOffline'), '')); break;
    case 'source-unavailable': body.append(unavailableCard()); break;
    case 'empty': body.append(lib.local ? pcEmpty('pcEmptyTitle', 'pcEmptyBody') : messageCard(ICONS.note, t('libEmptyTitle'), t('libEmptyBody'),
      el('button', { class: 'btn primary', text: t('libGoSync'), onclick: () => C.setTab('sync') }))); break;
    case 'nolinks': body.append(pcEmpty('pcNoLinksTitle', 'pcNoLinksBody')); break;
    case 'error': body.append(messageCard(ICONS.warn, C.errorMessage(lib.error), '',
      el('button', { class: 'btn', text: t('retry'), onclick: () => loadLibrary({ force: true }) }))); break;
    case 'ready': renderView(body); break;
    default: break;
  }
  renderFoot();
}

// The computer's library before there is any music to show: folders are linked in a HiBy's Sync tab.
function pcEmpty(title, text) {
  const d = C.selectedDevice();
  return messageCard(ICONS.folder, t(title), t(text),
    el('button', { class: 'btn primary', text: t('pcOpenSync'), disabled: !d, onclick: () => C.showDevice('sync') }));
}

export function messageCard(ic, title, text, ...actions) {
  return el('div', { class: 'card lib-message' }, el('div', { class: 'lib-message-icon' }, icon(ic)),
    el('h3', { text: title }), text ? el('p', { class: 'muted', text }) : null,
    actions.length ? el('div', { class: 'lib-message-actions' }, ...actions) : null);
}

function loadingCard() {
  const card = el('div', { class: 'card lib-loading', id: 'lib-loading' });
  fillLoading(card);
  return card;
}

function renderLoading() {
  const card = $('lib-loading');
  if (card) fillLoading(card);
}

function fillLoading(card) {
  const p = lib.progress;
  let text = t('libLoading');
  let frac = null;
  if (p?.op === 'pull' && p.total > 0) { text = t('libLoadingBytes', { done: fmtBytes(p.done), total: fmtBytes(p.total) }); frac = p.done / p.total; }
  else if (p?.op === 'index' && p.total > 0) { text = t('libIndexing', { n: fmtCount(p.total) }); frac = p.done / p.total; }
  else if (p?.op === 'list') text = t('libScanningList', { n: fmtCount(p.done) });
  else if (p?.op === 'tags' && p.total > 0) { text = t(lib.local ? 'pcScanningTags' : 'libScanning', { done: fmtCount(p.done), total: fmtCount(p.total) }); frac = p.done / p.total; }
  else if (p?.op === 'scan') text = t('pcScanningFolders', { n: fmtCount(p.done) });
  else if (lib.local) text = t('pcReading');
  card.replaceChildren(
    el('div', { class: 'lib-loading-art' }, el('span'), el('span'), el('span')),
    el('p', { class: 'lib-loading-text', text }),
    el('div', { class: `step-bar${frac === null ? ' wait' : ''}` }, el('span', { style: frac === null ? '' : `width:${(frac * 100).toFixed(1)}%` })));
}

function unavailableCard() {
  const used = C.appState.status?.sd ? C.appState.status.sd.sd_total - C.appState.status.sd.sd_free : 0;
  const est = scanEstimate(used);
  return el('div', { class: 'card lib-message unavailable' },
    el('div', { class: 'lib-message-icon warn' }, icon(ICONS.db)),
    el('h3', { text: t('libUnavailableTitle') }),
    el('p', { class: 'muted', text: t('libUnavailableBody', { min: est.minutes }) }),
    el('div', { class: 'lib-message-actions' },
      el('button', { class: 'btn primary', text: t('libScanInstead'), onclick: () => loadLibrary({ force: true, scan: true }) })));
}

function renderBanner() {
  const b = $('lib-banner');
  b.replaceChildren();
  if (lib.busy) {
    b.hidden = false;
    b.className = 'lib-banner busy';
    b.append(el('span', { class: 'spinner' }), el('span', { class: 'grow', text: lib.busy }));
    return;
  }
  if (lib.stale && lib.state === 'ready') {
    b.hidden = false;
    b.className = 'lib-banner stale';
    b.append(icon(ICONS.info, 'i banner-i'), el('span', { class: 'grow', text: t('libStale') }),
      el('button', { class: 'btn', text: t('libUpdate'), onclick: () => updateLibrary() }));
    return;
  }
  b.hidden = true;
}

function renderFoot() {
  const f = $('lib-foot');
  const idx = lib.index;
  if (!idx || lib.state !== 'ready') { f.textContent = ''; return; }
  const when = age(idx.time, Date.now() / 1000, t);
  const src = lib.local ? t('pcFoot', { age: when }) : idx.source === 'scan' ? t('libFromScan', { age: when }) : t('libFromDb', { age: when });
  const music = idx.tracks.filter((x) => !x.book);
  f.textContent = `${t('libSummary', { songs: fmtCount(music.length), albums: fmtCount(idx.albums.length), time: totalTime(music.reduce((s, x) => s + (x.duration || 0), 0)) })} — ${src}`;
}

function renderView(body) {
  const q = lib.query;
  switch (lib.view) {
    case 'artists': return renderArtists(body, q);
    case 'songs': case 'recent': return renderSongs(body, q);
    case 'books': return renderBooks(body, q);
    case 'playlists': return renderPlaylists(body, q);
    default: return renderAlbums(body, q);
  }
}

function noResults(body) {
  body.append(el('p', { class: 'muted center lib-none', text: t('libNoResults', { q: lib.query }) }));
}

// ---- albums: the grid that opens in place (iTunes 11) ------------------------------------------

function albumTracks(ai) { return lib.index.albums[ai].tracks; }

function renderAlbums(body, q) {
  const idx = lib.index;
  const shown = idx.albums.map((a, i) => i).filter((i) => matches(q, idx.albums[i].title, idx.albums[i].artist, idx.albums[i].genre));
  if (!shown.length) return noResults(body);
  const grid = el('div', { class: 'album-grid', role: 'list' });
  shown.forEach((ai, k) => {
    const a = idx.albums[ai];
    const item = el('div', {
      class: `album${lib.open === ai ? ' open' : ''}`, role: 'listitem', tabindex: '0', 'data-ai': String(ai),
      style: `animation-delay:${Math.min(k, 24) * 14}ms`,
      onclick: () => toggleAlbum(ai),
      onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggleAlbum(ai); } },
      oncontextmenu: (e) => { e.preventDefault(); trackMenu(e, albumTracks(ai)); },
    },
    cover(a.art, 'cover album-cover'),
    el('div', { class: 'album-title', text: a.title || t('unknownAlbum') }),
    el('div', { class: 'album-artist', text: a.artist || t('unknownArtist') }));
    grid.append(item);
  });
  body.append(grid);
  prefetchArt(shown.map((ai) => idx.albums[ai].art));
  if (lib.open !== null && shown.includes(lib.open)) requestAnimationFrame(() => placeDetail(grid, false));
}

function toggleAlbum(ai) {
  const grid = document.querySelector('.album-grid');
  const was = lib.open;
  lib.open = was === ai ? null : ai;
  lib.selected.clear();
  for (const n of grid.querySelectorAll('.album')) n.classList.toggle('open', Number(n.dataset.ai) === lib.open);
  const old = grid.querySelector('.album-detail');
  if (lib.open === null) {
    if (old) { old.classList.add('closing'); old.addEventListener('animationend', () => old.remove(), { once: true }); }
    return;
  }
  placeDetail(grid, was !== null, old);
}

// The detail goes after the last album of the open album's row, across the whole grid, with a
// notch under the album (iTunes 11).
function placeDetail(grid, quick, old = grid.querySelector('.album-detail')) {
  const item = grid.querySelector(`.album[data-ai="${lib.open}"]`);
  if (!item || !grid.isConnected) return;
  // the row from the grid's own column count (not from positions, which move while the
  // albums animate in or the detail itself changes the page height)
  const items = [...grid.querySelectorAll(':scope > .album')];
  const cols = Math.max(1, getComputedStyle(grid).gridTemplateColumns.split(' ').filter(Boolean).length);
  const k = items.indexOf(item);
  const last = items[Math.min(items.length - 1, Math.floor(k / cols) * cols + cols - 1)];
  const detail = albumDetail(lib.open);
  detail.style.setProperty('--notch', `${item.offsetLeft + item.offsetWidth / 2}px`);
  if (old && old.previousElementSibling === last) { old.replaceWith(detail); detail.classList.add('swap'); }
  else {
    old?.remove();
    last.after(detail);
    if (quick) detail.classList.add('swap');
  }
  tintDetail(detail);
  const r = detail.getBoundingClientRect();
  const content = document.getElementById('content');
  const cr = content.getBoundingClientRect();
  if (r.bottom > cr.bottom) content.scrollBy({ top: Math.min(r.bottom - cr.bottom + 16, r.top - cr.top - 120), behavior: 'smooth' });
}

function tintDetail(detail) {
  const a = lib.index.albums[Number(detail.dataset.ai)];
  const color = lib.local ? null : devLib.art.get(a?.art)?.color;
  if (!color) return;
  detail.style.setProperty('--tint', color);
  detail.classList.add('tinted', `on-${textOn(color)}`);
}

function albumDetail(ai) {
  const idx = lib.index;
  const a = idx.albums[ai];
  const tracks = a.tracks.map((i) => idx.tracks[i]);
  const dur = tracks.reduce((s, x) => s + (x.duration || 0), 0);
  const list = el('ol', { class: 'detail-tracks' });
  a.tracks.forEach((ti) => list.append(trackRow(ti, true)));
  const meta = [a.artist || t('unknownArtist'), a.year || null, a.genre || null].filter(Boolean).join(' · ');
  return el('div', { class: 'album-detail', 'data-ai': String(ai), role: 'region', 'aria-label': a.title },
    el('div', { class: 'detail-inner' },
      cover(a.art, 'cover detail-cover'),
      el('div', { class: 'detail-main' },
        el('div', { class: 'detail-head' },
          el('div', {},
            el('h2', { text: a.title || t('unknownAlbum') }),
            el('p', { class: 'detail-meta', text: meta })),
          el('button', { class: 'tool-btn detail-close', 'aria-label': t('cancel'), onclick: (e) => { e.stopPropagation(); toggleAlbum(ai); } }, icon(ICONS.close))),
        list,
        el('p', { class: 'detail-foot', text: `${t('songsCount', { n: tracks.length })}, ${totalTime(dur)} · ${quality(tracks[0])}` }))));
}

// One selectable song row (album detail, artist view, playlists).
function trackRow(ti, numbered, extraMenu) {
  const tr = lib.index.tracks[ti];
  const li = el('li', {
    class: `trk${lib.selected.has(ti) ? ' selected' : ''}`, 'data-ti': String(ti), tabindex: '-1',
    onclick: (e) => { e.stopPropagation(); select(ti, e); },
    ondblclick: (e) => { e.stopPropagation(); getInfo([ti]); },
    oncontextmenu: (e) => {
      e.preventDefault(); e.stopPropagation();
      if (!lib.selected.has(ti)) select(ti, {});
      trackMenu(e, [...lib.selected], extraMenu);
    },
  },
  el('span', { class: 'trk-n', text: typeof numbered === 'number' ? String(numbered) : numbered && tr.track ? String(tr.track) : '' }),
  el('span', { class: 'trk-t', text: tr.title }),
  numbered !== true ? el('span', { class: 'trk-a', text: tr.artist }) : null,
  el('span', { class: 'trk-d', text: fmtDuration(tr.duration) }));
  return li;
}

function select(ti, e) {
  const order = [...document.querySelectorAll('[data-ti]')].map((n) => Number(n.dataset.ti));
  if (e.shiftKey && lib.lastClicked !== null && order.includes(lib.lastClicked)) {
    const a = order.indexOf(lib.lastClicked); const b = order.indexOf(ti);
    if (!(e.ctrlKey || e.metaKey)) lib.selected.clear();
    for (const k of order.slice(Math.min(a, b), Math.max(a, b) + 1)) lib.selected.add(k);
  } else if (e.ctrlKey || e.metaKey) {
    if (lib.selected.has(ti)) lib.selected.delete(ti); else lib.selected.add(ti);
    lib.lastClicked = ti;
  } else {
    lib.selected = new Set([ti]);
    lib.lastClicked = ti;
  }
  for (const n of document.querySelectorAll('[data-ti]')) n.classList.toggle('selected', lib.selected.has(Number(n.dataset.ti)));
}

// ---- artists ------------------------------------------------------------------------------------

function renderArtists(body, q) {
  const idx = lib.index;
  const shown = idx.artists.map((a, i) => i).filter((i) => matches(q, idx.artists[i].name,
    ...idx.artists[i].albums.map((ai) => idx.albums[ai].title)));
  if (!shown.length) return noResults(body);
  if (!shown.includes(lib.artist)) lib.artist = shown[0];
  const list = el('ul', { class: 'artist-list', role: 'listbox' });
  for (const i of shown) {
    const a = idx.artists[i];
    list.append(el('li', {
      class: `artist-item${i === lib.artist ? ' selected' : ''}`, role: 'option', 'aria-selected': String(i === lib.artist),
      onclick: () => { lib.artist = i; lib.selected.clear(); renderLibrary(); },
    }, cover(idx.albums[a.albums[0]].art, 'cover artist-avatar'),
    el('span', { class: 'artist-name', text: a.name || t('unknownArtist') }),
    el('span', { class: 'artist-count', text: String(a.albums.length) })));
  }
  const a = idx.artists[lib.artist];
  prefetchArt([...a.albums.map((ai) => idx.albums[ai].art), ...shown.map((i) => idx.albums[idx.artists[i].albums[0]].art)]);
  const pane = el('div', { class: 'artist-pane' },
    el('div', { class: 'artist-head' },
      el('h2', { text: a.name || t('unknownArtist') }),
      el('p', { class: 'muted', text: `${t('albumsCount', { n: a.albums.length })}, ${t('songsCount', { n: a.tracks })}` })));
  for (const ai of a.albums) {
    const al = idx.albums[ai];
    const ol = el('ol', { class: 'detail-tracks plain' });
    al.tracks.forEach((ti) => ol.append(trackRow(ti, true)));
    pane.append(el('section', { class: 'artist-album' },
      cover(al.art, 'cover artist-album-cover'),
      el('div', { class: 'artist-album-main' },
        el('h3', { text: al.title || t('unknownAlbum') }),
        el('p', { class: 'muted small-text', text: [al.year || null, al.genre || null, quality(idx.tracks[al.tracks[0]])].filter(Boolean).join(' · ') }),
        ol)));
  }
  body.append(el('div', { class: 'artists' }, list, pane));
}

// ---- songs: a sortable virtual table ------------------------------------------------------------

const COLS = [
  ['title', 'colTitle'], ['duration', 'colTime'], ['artist', 'colArtist'], ['album', 'colAlbum'], ['genre', 'colGenre'], ['kind', 'colKind'],
];

function songRows(q) {
  const idx = lib.index;
  let rows = idx.tracks.map((x, i) => i).filter((i) => !idx.tracks[i].book &&
    matches(q, idx.tracks[i].title, idx.tracks[i].artist, idx.tracks[i].album, idx.tracks[i].genre));
  const { key, dir } = lib.view === 'recent' ? { key: 'ctime', dir: -1 } : lib.sort;
  const val = (i) => {
    const x = idx.tracks[i];
    return key === 'kind' ? quality(x) : x[key];
  };
  rows = rows.sort((a, b) => {
    const x = val(a); const y = val(b);
    const c = typeof x === 'number' && typeof y === 'number' ? x - y : compareNames(String(x ?? ''), String(y ?? ''));
    if (c) return c * dir;
    const ta = idx.tracks[a]; const tb = idx.tracks[b];
    return compareNames(ta.album, tb.album) || ta.track - tb.track;
  });
  return rows;
}

function renderSongs(body, q) {
  const rows = songRows(q);
  if (!rows.length) return noResults(body);
  const head = el('div', { class: 'songs-head', role: 'row' });
  for (const [key, label] of COLS) {
    const active = lib.view !== 'recent' && lib.sort.key === key;
    head.append(el('button', {
      class: `col col-${key}${active ? ` sorted ${lib.sort.dir > 0 ? 'asc' : 'desc'}` : ''}`, role: 'columnheader',
      onclick: () => {
        if (lib.view === 'recent') lib.view = 'songs';
        lib.sort = { key, dir: lib.sort.key === key ? -lib.sort.dir : 1 };
        renderLibrary();
      },
    }, el('span', { text: t(label) })));
  }
  const scroll = el('div', { class: 'songs-scroll', role: 'grid', tabindex: '0' });
  const spacer = el('div', { class: 'songs-spacer' });
  scroll.append(spacer);
  const draw = () => {
    const w = virtualWindow(scroll.scrollTop, scroll.clientHeight || 600, ROW_H, rows.length);
    spacer.style.height = `${w.height}px`;
    const frag = document.createDocumentFragment();
    for (let k = w.start; k < w.end; k++) frag.append(songRow(rows[k], k));
    spacer.replaceChildren(frag);
  };
  scroll.addEventListener('scroll', () => requestAnimationFrame(draw), { passive: true });
  body.append(el('div', { class: 'songs' }, head, scroll));
  requestAnimationFrame(draw);
}

function songRow(ti, k) {
  const x = lib.index.tracks[ti];
  return el('div', {
    class: `song${k % 2 ? ' odd' : ''}${lib.selected.has(ti) ? ' selected' : ''}`, role: 'row', 'data-ti': String(ti),
    style: `top:${k * ROW_H}px`,
    onclick: (e) => select(ti, e),
    ondblclick: () => getInfo([ti]),
    oncontextmenu: (e) => { e.preventDefault(); if (!lib.selected.has(ti)) select(ti, {}); trackMenu(e, [...lib.selected]); },
  },
  el('span', { class: 'col col-title', text: x.title }),
  el('span', { class: 'col col-duration', text: fmtDuration(x.duration) }),
  el('span', { class: 'col col-artist', text: x.artist }),
  el('span', { class: 'col col-album', text: x.album }),
  el('span', { class: 'col col-genre', text: x.genre }),
  el('span', { class: 'col col-kind', text: quality(x) }));
}

// ---- audiobooks -----------------------------------------------------------------------------------

function renderBooks(body, q) {
  const idx = lib.index;
  const shown = idx.books.filter((b) => matches(q, b.name, idx.tracks[b.tracks[0]]?.artist));
  if (!shown.length) return noResults(body);
  const grid = el('div', { class: 'album-grid books' });
  shown.forEach((b, k) => {
    const author = idx.tracks[b.tracks[0]]?.artist ?? '';
    grid.append(el('div', {
      class: 'album book', style: `animation-delay:${Math.min(k, 24) * 14}ms`, tabindex: '0',
      oncontextmenu: (e) => { e.preventDefault(); trackMenu(e, b.tracks); },
      ondblclick: () => getInfo(b.tracks),
    },
    cover(idx.tracks[b.tracks[0]].path, 'cover album-cover'),
    el('div', { class: 'album-title', text: b.name }),
    el('div', { class: 'album-artist', text: author }),
    el('div', { class: 'book-meta', text: `${t('chapters', { n: b.tracks.length })} · ${totalTime(b.duration)}` })));
  });
  body.append(grid);
  prefetchArt(shown.map((b) => idx.tracks[b.tracks[0]].path));
}

// ---- playlists --------------------------------------------------------------------------------------

async function loadPlaylists() {
  if (lib.local || lib.playlists || !device()) return;
  lib.playlists = new Map();
  const entries = [];
  const final = await C.runCli('playlist-ls', { device: spec() }, {
    quiet: true, onEvent: (ev) => { if (ev.event === 'entry') entries.push(ev); },
  });
  if (final.ok) for (const e of entries) lib.playlists.set(e.path, { name: e.name, path: e.path, items: e.items ?? [] });
  if (lib.view === 'playlists') renderLibrary();
}

function playlistList() {
  const fromIndex = (lib.index?.playlists ?? []).map((p) => ({ name: p.name, path: p.path, items: null }));
  const all = new Map(fromIndex.map((p) => [p.path, p]));
  for (const [k, v] of lib.playlists ?? []) all.set(k, v);
  return [...all.values()].sort((a, b) => compareNames(a.name, b.name));
}

function renderPlaylists(body, q) {
  loadPlaylists();
  const pls = playlistList();
  if (!pls.length) { body.append(messageCard(ICONS.note, t(lib.local ? 'pcPlaylistsNone' : 'playlistsNone'), lib.local ? t('pcPlaylistsBody') : '')); return; }
  if (!pls.some((p) => p.path === lib.playlist)) lib.playlist = pls[0].path;
  const list = el('ul', { class: 'artist-list playlist-list', role: 'listbox' });
  for (const p of pls) {
    list.append(el('li', {
      class: `artist-item${p.path === lib.playlist ? ' selected' : ''}`, role: 'option',
      onclick: () => { lib.playlist = p.path; lib.selected.clear(); renderLibrary(); },
    }, el('span', { class: 'pl-icon' }, icon(ICONS.note)), el('span', { class: 'artist-name', text: p.name }),
    el('span', { class: 'artist-count', text: p.items ? String(p.items.length) : '' })));
  }
  const p = pls.find((x) => x.path === lib.playlist);
  const byPath = new Map(lib.index.tracks.map((x, i) => [x.path.toLowerCase(), i]));
  const tis = (p.items ?? []).map((x) => byPath.get(x.toLowerCase())).filter((x) => x !== undefined)
    .filter((ti) => matches(q, lib.index.tracks[ti].title, lib.index.tracks[ti].artist, lib.index.tracks[ti].album));
  const ol = el('ol', { class: 'detail-tracks plain playlist-tracks' });
  const extra = [{ label: t('removeFromPlaylist'), action: (sel) => removeFromPlaylist(p, sel) }];
  tis.forEach((ti, k) => ol.append(trackRow(ti, k + 1, extra)));
  const dur = tis.reduce((s, ti) => s + (lib.index.tracks[ti].duration || 0), 0);
  const pane = el('div', { class: 'artist-pane' },
    el('div', { class: 'artist-head row' },
      el('div', {}, el('h2', { text: p.name }),
        el('p', { class: 'muted', text: p.items ? `${t('songsCount', { n: tis.length })}, ${totalTime(dur)}` : t('libLoading') })),
      el('button', { class: 'btn danger', text: t('deletePlaylist'), onclick: () => deletePlaylist(p) })),
    p.items && !tis.length ? el('p', { class: 'muted', text: t('playlistEmpty') }) : ol);
  body.append(el('div', { class: 'artists' }, list, pane));
}

// ---- context menu ---------------------------------------------------------------------------------

function closeMenu() {
  const m = $('ctx-menu');
  if (m) m.hidden = true;
}

function showMenu(x, y, items) {
  const m = $('ctx-menu');
  m.replaceChildren();
  const build = (into, list) => {
    for (const it of list) {
      if (it === '-') { into.append(el('div', { class: 'ctx-sep', role: 'separator' })); continue; }
      const b = el('button', {
        class: `ctx-item${it.danger ? ' danger' : ''}${it.sub ? ' has-sub' : ''}`, role: 'menuitem', disabled: it.disabled || false,
        onclick: (e) => { if (it.sub) { e.stopPropagation(); return; } closeMenu(); it.action?.(); },
      }, el('span', { text: it.label }));
      if (it.sub) {
        const sub = el('div', { class: 'ctx-sub', role: 'menu' });
        build(sub, it.sub);
        b.append(sub);
      }
      into.append(b);
    }
  };
  build(m, items);
  m.hidden = false;
  const w = m.offsetWidth; const h = m.offsetHeight;
  m.style.left = `${Math.min(x, window.innerWidth - w - 8)}px`;
  m.style.top = `${Math.min(y, window.innerHeight - h - 8)}px`;
  m.classList.toggle('flip', x + w + 220 > window.innerWidth);
  m.querySelector('.ctx-item')?.focus();
}

function trackMenu(e, tis, extra = []) {
  if (!tis.length) return;
  // the computer's files: read-only here (tag editing is the Tidy up milestone)
  if (lib.local) { showMenu(e.clientX, e.clientY, [{ label: t('getInfo'), action: () => getInfo(tis) }]); return; }
  const pls = playlistList();
  showMenu(e.clientX, e.clientY, [
    { label: t('getInfo'), action: () => getInfo(tis) },
    { label: t('addToPlaylist'), sub: [{ label: t('newPlaylist'), action: () => addToPlaylist(tis, null) }, ...(pls.length ? ['-'] : []),
      ...pls.map((p) => ({ label: p.name, action: () => addToPlaylist(tis, p) }))] },
    ...extra.map((x) => ({ label: x.label, action: () => x.action(tis) })),
    '-',
    { label: t('rescan'), action: () => rescanFull() },
    '-',
    { label: t('delFromHiby'), danger: true, action: () => deleteTracks(tis) },
  ]);
}

// ---- library actions ---------------------------------------------------------------------------------

function setBusy(text) { lib.busy = text; renderBanner(); }

async function deleteTracks(tis) {
  const idx = lib.index;
  const names = tis.slice(0, 8).map((ti) => `${idx.tracks[ti].title} — ${idx.tracks[ti].artist}`);
  const r = await C.sheet({
    title: t('delSongsTitle', { n: tis.length }), body: t('delSongsBody'), okText: t('deleteAction'), danger: true,
    extra: (c) => {
      const ul = el('ul', { class: 'sheet-list' });
      for (const n of names) ul.append(el('li', { text: n }));
      if (tis.length > names.length) ul.append(el('li', { class: 'muted', text: t('delAndMore', { n: tis.length - names.length }) }));
      c.append(ul);
    },
  });
  if (!r.ok) return;
  const paths = tis.map((ti) => idx.tracks[ti].path);
  setBusy(t('libBusyDeleting', { n: tis.length }));
  const deleted = [];
  for (let i = 0; i < paths.length; i += 500) {
    const final = await C.runCli('rm', { device: spec(), paths: paths.slice(i, i + 500), yes: true, pruneEmpty: true }, { title: t('libBusyDeleting', { n: tis.length }) });
    if (!final.ok) { toast(C.errorMessage(final), 'fail'); break; }
    deleted.push(...(final.deleted ?? []));
  }
  setBusy(null);
  if (!deleted.length) return;
  const gone = new Set(deleted);
  lib.index = groupIndex(idx.tracks.filter((x) => !gone.has(x.path)), { source: idx.source, revision: idx.revision, time: idx.time, playlists: idx.playlists });
  lib.selected.clear();
  lib.open = null;
  lib.stale = true;
  lib.stalePaths.push(...deleted);
  toast(t('libDeleted', { n: deleted.length }), 'ok');
  setIndex(lib.index);
}

async function updateLibrary() {
  const paths = lib.stalePaths;
  const final = paths.length && paths.length <= 256
    ? await C.runCli('lib-rescan', { device: spec(), paths }, { title: t('libUpdate') })
    : await C.runCli('lib-rescan', { device: spec(), full: true }, { title: t('libUpdate') });
  if (final.ok) {
    lib.stale = false; lib.stalePaths = [];
    toast(t('rescanDone'), 'ok');
    renderBanner();
  } else rescanHelp(final);
}

async function rescanFull() {
  const final = await C.runCli('lib-rescan', { device: spec(), full: true }, { title: t('rescan') });
  if (final.ok) { toast(t('rescanDone'), 'ok'); return; }
  rescanHelp(final);
}

function rescanHelp(final) {
  if (final.btp?.name === 'E_UNSUPPORTED') {
    C.sheet({ title: t('rescanUnsupportedTitle'), body: t('rescanUnsupported'), okText: t('ok'), noCancel: true,
      extra: (c) => c.append(el('div', { class: 'howto' }, el('span', { class: 'howto-step', text: t('howtoSettings') }), el('span', { text: '›' }),
        el('span', { class: 'howto-step', text: t('howtoUpdate') }))) });
  } else toast(C.errorMessage(final), 'fail');
}

async function addToPlaylist(tis, p) {
  let name = p?.name;
  if (!p) {
    const r = await C.sheet({
      title: t('newPlaylistTitle'), body: t('newPlaylistPrompt'), okText: t('create'), input: '',
      validate: (v) => (!v.trim() || v.includes('/') ? t('bad_empty') : null),
    });
    if (!r.ok) return;
    name = r.value.trim();
  }
  const paths = tis.map((ti) => lib.index.tracks[ti].path);
  // an existing playlist goes by its file, so .m3u files and an .m3u next to an .m3u8 work
  const target = p ? { path: p.path } : { name };
  const final = await C.runCli('playlist-add', { device: spec(), ...target, paths, create: !p }, { title: t('addToPlaylist') });
  if (!final.ok) { toast(C.errorMessage(final), 'fail'); return; }
  toast(t('addedToPlaylist', { n: final.added, name }), 'ok');
  lib.playlists = null;
  if (lib.view === 'playlists') renderLibrary();
}

async function removeFromPlaylist(p, tis) {
  const final = await C.runCli('playlist-rm', { device: spec(), path: p.path, paths: tis.map((ti) => lib.index.tracks[ti].path) }, { title: t('removeFromPlaylist') });
  if (!final.ok) { toast(C.errorMessage(final), 'fail'); return; }
  lib.playlists = null;
  lib.selected.clear();
  renderLibrary();
}

async function deletePlaylist(p) {
  const r = await C.sheet({ title: t('deletePlaylist').replace('…', ''), body: t('deletePlaylistBody', { name: p.name }), okText: t('deleteAction'), danger: true });
  if (!r.ok) return;
  const final = await C.runCli('playlist-rm', { device: spec(), path: p.path }, { title: t('deletePlaylist') });
  if (!final.ok) { toast(C.errorMessage(final), 'fail'); return; }
  if (lib.index) lib.index.playlists = lib.index.playlists.filter((x) => x.path !== p.path);
  lib.playlists = null;
  lib.playlist = null;
  renderLibrary();
}

function getInfo(tis) {
  const idx = lib.index;
  const x = idx.tracks[tis[0]];
  C.sheet({
    title: tis.length === 1 ? x.title : t('infoSongs', { n: tis.length }), body: tis.length === 1 ? `${x.artist} — ${x.album}` : '',
    okText: t('ok'), noCancel: true,
    extra: (c) => {
      const head = el('div', { class: 'info-head' }, cover(x.path, 'cover info-cover'));
      const dl = el('dl', { class: 'facts info' });
      const add = (k, v) => { if (v !== undefined && v !== null && v !== '' && v !== 0) dl.append(el('dt', { text: k }), el('dd', { text: String(v) })); };
      if (tis.length === 1) {
        add(t('infoAlbumArtist'), x.albumArtist); add(t('infoGenre'), x.genre); add(t('infoYear'), x.year);
        add(t('infoTrack'), x.track); add(t('infoDisc'), x.disc); add(t('infoDuration'), fmtDuration(x.duration));
        add(t('infoFormat'), quality(x)); add(t('infoSize'), fmtBytes(x.size));
        add(t('infoAdded'), x.ctime ? new Intl.DateTimeFormat(lang(), { dateStyle: 'medium' }).format(new Date(x.ctime * 1000)) : '');
        add(t('infoPath'), x.path);
      } else {
        add(t('infoSize'), fmtBytes(tis.reduce((s, ti) => s + (idx.tracks[ti].size || 0), 0)));
        add(t('infoDuration'), totalTime(tis.reduce((s, ti) => s + (idx.tracks[ti].duration || 0), 0)));
      }
      head.append(dl);
      c.append(head, el('p', { class: 'muted small-text', text: t(lib.local ? 'pcInfoReadOnly' : 'infoReadOnly') }));
    },
  });
}

async function fixCovers() {
  setBusy(t('fixCoversChecking'));
  const entries = [];
  const dry = await C.runCli('art-fix', { device: spec(), dryRun: true }, {
    title: t('fixCoversChecking'), onEvent: (ev) => { if (ev.event === 'entry') entries.push(ev); },
  });
  setBusy(null);
  if (!dry.ok) { toast(C.errorMessage(dry), 'fail'); return; }
  if (!dry.files) { toast(t('fixCoversNone'), 'ok'); return; }
  const r = await C.sheet({
    title: t('fixCoversTitle', { n: dry.albums }), body: t('fixCoversBody', { n: dry.albums, files: dry.files }), okText: t('fixCoversAction'),
    extra: (c) => {
      const ul = el('ul', { class: 'sheet-list' });
      for (const e of entries.slice(0, 6)) ul.append(el('li', { text: e.path }));
      if (entries.length > 6) ul.append(el('li', { class: 'muted', text: t('delAndMore', { n: entries.length - 6 }) }));
      c.append(ul);
    },
  });
  if (!r.ok) return;
  setBusy(t('fixingCovers'));
  const final = await C.runCli('art-fix', { device: spec() }, { title: t('fixingCovers') });
  setBusy(null);
  if (final.ok) {
    toast(t('fixCoversDone', { n: final.fixed }), 'ok');
    devLib.art = new Map();
    renderLibrary();
  } else toast(C.errorMessage(final), 'fail');
}

async function resetCoverCache() {
  const r = await C.sheet({ title: t('resetCoverCache'), body: t('resetCoverCacheBody'), okText: t('resetAction') });
  if (!r.ok) return;
  const final = await C.runCli('cover-cache-reset', { device: spec() }, { title: t('resetCoverCache') });
  if (final.ok) toast(t('resetCoverCacheDone', { n: final.deleted }), 'ok');
  else toast(C.errorMessage(final), 'fail');
}

// ---- wiring -------------------------------------------------------------------------------------

export function initMedia(ctx) {
  C = ctx;
  for (const b of document.querySelectorAll('#lib-views button')) {
    b.addEventListener('click', () => setView(b.dataset.view));
  }
  for (const b of document.querySelectorAll('.side-lib')) {
    b.addEventListener('click', () => C.showComputer(b.dataset.libview));
  }
  const search = $('lib-search');
  let timer = null;
  search.addEventListener('input', () => { clearTimeout(timer); timer = setTimeout(() => { lib.query = search.value; lib.open = null; renderLibrary(); }, 120); });
  $('lib-refresh').addEventListener('click', () => loadLibrary({ force: true }));
  $('lib-covers').addEventListener('click', (e) => {
    const r = e.currentTarget.getBoundingClientRect();
    showMenu(r.left, r.bottom + 4, [
      { label: t('fixCovers'), action: () => fixCovers() },
      { label: t('resetCoverCache'), action: () => resetCoverCache() },
    ]);
  });
  document.addEventListener('click', (e) => { if (!e.target.closest('#ctx-menu')) closeMenu(); });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeMenu();
    if (!libraryOnScreen() || document.querySelector('dialog[open]') || e.target.closest?.('input, select')) return;
    if (e.key === 'Delete' && lib.selected.size && !lib.local) deleteTracks([...lib.selected]);
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') { e.preventDefault(); $('lib-search').focus(); }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'i' && lib.selected.size) { e.preventDefault(); getInfo([...lib.selected]); }
  });
  document.getElementById('content').addEventListener('scroll', closeMenu, { passive: true });
  window.addEventListener('resize', () => {
    if (libraryOnScreen() && lib.view === 'albums' && lib.open !== null) {
      const grid = document.querySelector('.album-grid');
      if (grid) placeDetail(grid, true);
    }
  });
}

const libraryOnScreen = () => (lib.local ? C.appState.view === 'computer' : C.appState.view !== 'computer' && C.appState.tab === 'library');

// Shows one library: the panel (toolbar, views, footer) moves into the page that shows it.
function useLibrary(L) {
  const root = $('lib-root');
  const slot = L.local ? $('pc-slot') : document.querySelector('.tab-panel[data-panel="library"]');
  if (root && slot && root.parentElement !== slot) slot.append(root);
  if (lib === L) return;
  lib = L;
  $('lib-search').value = lib.query;
  closeMenu();
}

function setView(v) {
  lib.view = v;
  lib.selected.clear();
  closeMenu();
  C.saveLocation();
  renderLibrary();
}

export function mediaLocation() {
  if (C.appState.view === 'computer') return `/${pcLib.view}`;
  return C.appState.tab === 'library' && devLib.view !== 'albums' ? `/${devLib.view}` : '';
}

export function readMediaLocation(rest, computer = false) {
  if (['albums', 'artists', 'songs', 'books', 'playlists', 'recent'].includes(rest)) (computer ? pcLib : devLib).view = rest;
}

// The selected HiBy changed (or was refreshed).
export function mediaDeviceChanged() {
  if (devLib.demo && devLib.loading) return; // a fixed demo picture
  devLib.deviceId = null;
  devLib.index = null;
  devLib.state = 'idle';
}

export function mediaTabOpened(tab) {
  if (tab === 'library') { useLibrary(devLib); renderLibrary(); loadLibrary(); }
}

// The sidebar's Library: the computer's music (design 10.1).
export function computerOpened(view) {
  useLibrary(pcLib);
  if (view) pcLib.view = view;
  renderComputerHead();
  renderLibrary();
  loadComputer();
}

// The page header of the computer's library: which folders it shows.
function renderComputerHead() {
  const sub = $('pc-sub');
  if (!sub) return;
  const idx = pcLib.index;
  const roots = (idx?.roots ?? []).map((r) => String(r).replace(/[\\/]+$/, '').split(/[\\/]/).pop()).filter(Boolean);
  sub.textContent = roots.length ? t('pcFolders', { names: roots.join(', ') }) : t('pcSub');
}

// After a sync or a link change, the computer's catalogue may be new.
export function libraryChanged() {
  devLib.index = null;
  devLib.state = 'idle';
}

// The app re-renders its page on every device refresh (every 5 s): the libraries are only rebuilt
// when something they show changed, so animations, scroll positions, an open album and a
// half-typed filter stay as they are.
let mediaKey = '';
export function renderMedia() {
  const d = device();
  const key = JSON.stringify([lang(), d?.id, d?.reachable, C.appState.tab, C.appState.view, !!C.appState.status]);
  if (key === mediaKey) return;
  mediaKey = key;
  if (C.appState.view === 'computer') { useLibrary(pcLib); renderComputerHead(); renderLibrary(); }
  else if (C.appState.tab === 'library') { useLibrary(devLib); renderLibrary(); }
}

// Demo mode only (?demo=NAME&...): fixed pictures of the Library states, for screenshots and
// design review. The rest of the page is the real (mock) flow.
export async function demoMedia(name, params) {
  if (name.startsWith('pc-')) {
    // the computer's library: a demo folder (the fake byos-cli invents its music)
    try { await C.api('/api/pick-folder', { method: 'POST', body: {} }); } catch { /* demo */ }
    const v = params.get('libview'); if (v) pcLib.view = v;
    if (name === 'pc-open') pcLib.afterLoad = () => { pcLib.open = Number(params.get('open') ?? 2); renderLibrary(); };
    return 'computer';
  }
  if (name.startsWith('lib-')) {
    lib.demo = name;
    const v = params.get('libview'); if (v) lib.view = v;
    if (name === 'lib-unavailable') { lib.state = 'source-unavailable'; lib.deviceId = '0123456789abcdef'; lib.loading = true; return 'library'; }
    if (name === 'lib-scanning') {
      lib.state = 'loading'; lib.loading = true; lib.deviceId = '0123456789abcdef';
      lib.progress = { op: 'tags', done: 1240, total: 3900 };
      return 'library';
    }
    if (name === 'lib-empty') { lib.state = 'empty'; lib.loading = true; lib.deviceId = '0123456789abcdef'; lib.index = { tracks: [], albums: [], artists: [], books: [], playlists: [] }; return 'library'; }
    const after = () => {
      const q = params.get('q'); if (q) { lib.query = q; $('lib-search').value = q; }
      const open = params.get('open'); if (open !== null) lib.open = Number(open);
      if (name === 'lib-stale') { lib.stale = true; lib.stalePaths = ['Music/x.flac']; }
      const artist = params.get('artist'); if (artist !== null) lib.artist = Number(artist);
      renderLibrary();
      const first = lib.index.albums[Number(open ?? 0)]?.tracks ?? [];
      if (params.get('menu') === '1') {
        lib.selected = new Set(first.slice(1, 3));
        renderLibrary();
        loadPlaylists().then(() => setTimeout(() => {
          trackMenu({ clientX: 760, clientY: 520 }, [...lib.selected]);
          const sub = document.querySelector('.ctx-item.has-sub');
          if (sub) sub.classList.add('demo-open');
        }, 100));
      }
      if (params.get('info') === '1') getInfo([first[0]]);
      if (params.get('delete') === '1') deleteTracks(first.slice(0, 3));
    };
    lib.afterLoad = after;
    return 'library';
  }
  return null;
}

// Called by the demo once the real flow reached its state.
export function demoAfter() {
  for (const L of [devLib, pcLib]) {
    if (L.afterLoad && L.state === 'ready' && L === lib) { const f = L.afterLoad; L.afterLoad = null; f(); }
  }
  return false;
}

export function mediaState() { return { lib, devLib, pcLib }; }
