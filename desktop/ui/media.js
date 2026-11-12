// The Library and Music tabs (docs/DESKTOP_APP.md): the HiBy's library as iTunes 11 showed
// one (albums grid that opens in place, artists, songs, audiobooks, playlists) and the Music
// tab's sync of this computer's music. Every device operation is a byos-cli job; names from the
// HiBy and from music files only ever go into the page with textContent.

import {
  age, compareNames, fmtDuration, fmtLeft, groupIndex, libraryState, matches, planFreeAfter, planSummary, quality,
  scanEstimate, secondsLeft, selectionJson, syncOutcome, textOn, virtualWindow,
} from '/media-format.js';

let C; // the app's helpers (initMedia)
const ROW_H = 28;

const lib = {
  deviceId: null, index: null, state: 'idle', progress: null, error: null, view: 'albums', query: '', open: null,
  artist: 0, playlist: null, playlists: null, sort: { key: 'title', dir: 1 }, selected: new Set(), lastClicked: null,
  stale: false, stalePaths: [], busy: null, art: new Map(), artQueue: new Set(), artTimer: null, artRunning: false,
  loading: false, demo: null,
};

const mus = {
  sources: null, catalog: null, state: 'idle', sel: null, opts: null, plan: null, planning: false, planTimer: null,
  progress: {}, final: null, job: null, failed: [], error: null, filter: '', dropping: null, demo: null, planToken: 0,
};

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

function toast(text, kind = '') {
  const n = $('toast');
  n.textContent = text;
  n.className = `toast ${kind}`;
  n.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { n.hidden = true; }, 3800);
}

const SVG_NS = 'http://www.w3.org/2000/svg';
function icon(d, cls = 'i') {
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
const ICONS = {
  note: ['M9 18V5l11-2v13', 'M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0Z', 'M20 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0Z'],
  folder: ['M3 7.5A1.5 1.5 0 0 1 4.5 6H9l2 2h8.5A1.5 1.5 0 0 1 21 9.5v8A1.5 1.5 0 0 1 19.5 19h-15A1.5 1.5 0 0 1 3 17.5Z'],
  check: ['M5 12.5 10 17.5 19 7'],
  warn: ['M12 4 21 19H3Z', 'M12 10v4', 'M12 16.5v.5'],
  info: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z', 'M12 11v6', 'M12 7.5v.5'],
  close: ['M7 7l10 10M17 7 7 17'],
  db: ['M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3Z', 'M4 6v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6', 'M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3'],
  sd: ['M7 3h8l4 4v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1Z', 'M10 3v4M13 3v4M16 4v3'],
  sync: ['M20 12a8 8 0 0 1-13.7 5.7', 'M4 12a8 8 0 0 1 13.7-5.7', 'M18 3v4h-4', 'M6 21v-4h4'],
};

// ---- album art: lazy, batched lib-art jobs ------------------------------------------------------

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
  paintArt(box);
  if (!lib.art.has(path) && device()?.reachable !== false) artObserve(box);
  return box;
}

function paintArt(box) {
  const a = lib.art.get(box.dataset.art);
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
  if (!path || lib.art.has(path)) return;
  lib.artQueue.add(path);
  clearTimeout(lib.artTimer);
  lib.artTimer = setTimeout(flushArt, 40);
}

async function flushArt() {
  if (lib.artRunning || !lib.artQueue.size || !device()) return;
  const paths = [...lib.artQueue].slice(0, 64);
  for (const p of paths) lib.artQueue.delete(p);
  lib.artRunning = true;
  const final = await C.runCli('lib-art', { device: spec(), paths, size: 256 }, { quiet: true });
  lib.artRunning = false;
  if (final.ok) {
    for (const it of final.items ?? []) lib.art.set(it.path, it.none ? { none: true } : { key: it.key, color: it.color });
    for (const box of document.querySelectorAll('.art[data-art]')) paintArt(box);
    const open = document.querySelector('.album-detail');
    if (open) tintDetail(open);
  } else {
    for (const p of paths) lib.art.set(p, { none: true });
  }
  if (lib.artQueue.size) flushArt();
}

// ---- library loading -----------------------------------------------------------------------------

async function fetchIndex(id) {
  try { return await C.api(`/api/library/${id}`); } catch { return null; }
}

async function loadLibrary({ force = false, scan = false } = {}) {
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

function setIndex(idx) {
  // a background refresh that found the same library only updates the footer (no flicker)
  const old = lib.index;
  if (old && lib.state === 'ready' && idx.revision && old.revision === idx.revision && old.tracks.length === idx.tracks.length) {
    lib.index = { ...old, time: idx.time };
    renderFoot();
    return;
  }
  lib.index = idx;
  lib.state = libraryState({ ok: true }, idx);
  if (lib.artist >= idx.artists.length) lib.artist = 0;
  if (lib.open !== null && lib.open >= idx.albums.length) lib.open = null;
  renderLibrary();
  demoAfter();
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
    b.classList.toggle('selected', C.appState.tab === 'library' && b.dataset.libview === lib.view);
  }
  const ready = lib.state === 'ready';
  $('lib-search').disabled = !ready;
  $('lib-search').placeholder = t('libSearch');
  $('lib-covers').disabled = !ready || device()?.reachable === false;
  $('lib-refresh').disabled = lib.loading || device()?.reachable === false;
  renderBanner();
  const body = $('lib-body');
  body.replaceChildren();
  body.className = `lib-body view-${lib.view}`;
  switch (lib.state) {
    case 'loading': body.append(loadingCard()); break;
    case 'offline': body.append(messageCard(ICONS.sd, t('libOffline'), '')); break;
    case 'source-unavailable': body.append(unavailableCard()); break;
    case 'empty': body.append(messageCard(ICONS.note, t('libEmptyTitle'), t('libEmptyBody'),
      el('button', { class: 'btn primary', text: t('libGoMusic'), onclick: () => C.setTab('music') }))); break;
    case 'error': body.append(messageCard(ICONS.warn, C.errorMessage(lib.error), '',
      el('button', { class: 'btn', text: t('retry'), onclick: () => loadLibrary({ force: true }) }))); break;
    case 'ready': renderView(body); break;
    default: break;
  }
  renderFoot();
}

function messageCard(ic, title, text, ...actions) {
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
  else if (p?.op === 'tags' && p.total > 0) { text = t('libScanning', { done: fmtCount(p.done), total: fmtCount(p.total) }); frac = p.done / p.total; }
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
  const src = idx.source === 'scan' ? t('libFromScan', { age: when }) : t('libFromDb', { age: when });
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
  const color = lib.art.get(a?.art)?.color;
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
  if (lib.playlists || !device()) return;
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
  if (!pls.length) { body.append(messageCard(ICONS.note, t('playlistsNone'), '')); return; }
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
  const final = await C.runCli('playlist-add', { device: spec(), name, paths, create: !p }, { title: t('addToPlaylist') });
  if (!final.ok) { toast(C.errorMessage(final), 'fail'); return; }
  toast(t('addedToPlaylist', { n: final.added, name }), 'ok');
  lib.playlists = null;
  if (lib.view === 'playlists') renderLibrary();
}

async function removeFromPlaylist(p, tis) {
  const final = await C.runCli('playlist-rm', { device: spec(), name: p.name, paths: tis.map((ti) => lib.index.tracks[ti].path) }, { title: t('removeFromPlaylist') });
  if (!final.ok) { toast(C.errorMessage(final), 'fail'); return; }
  lib.playlists = null;
  lib.selected.clear();
  renderLibrary();
}

async function deletePlaylist(p) {
  const r = await C.sheet({ title: t('deletePlaylist').replace('…', ''), body: t('deletePlaylistBody', { name: p.name }), okText: t('deleteAction'), danger: true });
  if (!r.ok) return;
  const final = await C.runCli('playlist-rm', { device: spec(), name: p.name }, { title: t('deletePlaylist') });
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
      c.append(head, el('p', { class: 'muted small-text', text: t('infoReadOnly') }));
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
    lib.art = new Map();
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

// ---- Music tab: this computer's music and the sync ---------------------------------------------

function defaultSel() {
  const s = storage('byos.music.sel');
  return {
    all: s?.all ?? true, artists: new Set(s?.artists ?? []), albums: new Set(s?.albums ?? []), genres: new Set(s?.genres ?? []),
    books: s?.books ?? true,
  };
}
function defaultOpts() {
  const o = storage('byos.music.opts');
  return { lyrics: o?.lyrics ?? true, covers: o?.covers ?? true, remove: o?.remove ?? false, dest: o?.dest ?? 'Music', layout: o?.layout ?? 'artist-album' };
}
function saveSel() {
  storage('byos.music.sel', { all: mus.sel.all, artists: [...mus.sel.artists], albums: [...mus.sel.albums], genres: [...mus.sel.genres], books: mus.sel.books });
  storage('byos.music.opts', mus.opts);
}

async function loadMusic({ rescan = false } = {}) {
  mus.sel ??= defaultSel();
  mus.opts ??= defaultOpts();
  if (['applying', 'scanning'].includes(mus.state)) return;
  try { mus.sources = (await C.api('/api/sources')).sources; } catch { mus.sources = []; }
  if (!mus.sources.length) { mus.state = 'no-sources'; mus.catalog = null; renderMusic(); return; }
  if (!rescan && !mus.catalog) {
    try {
      const cat = await C.api('/api/catalog');
      const roots = new Set((cat.roots ?? []).map((r) => r.replace(/\\/g, '/').toLowerCase()));
      const same = mus.sources.filter((s) => s.exists).every((s) => roots.has(s.path.replace(/\\/g, '/').toLowerCase()));
      if (same && cat.tracks) mus.catalog = cat;
    } catch { /* not scanned yet */ }
  }
  if (!mus.catalog || rescan) { await scanMusic(); return; }
  if (!['plan-ready', 'planning', 'done', 'partial', 'library-pending', 'cancelled', 'error'].includes(mus.state)) {
    mus.state = 'select';
    schedulePlan(0);
  }
  renderMusic();
}

async function scanMusic() {
  mus.state = 'scanning';
  mus.progress = {};
  renderMusic();
  const final = await C.runCli('music-scan', {}, {
    title: t('scanJob'),
    onEvent: (ev) => { if (ev.event === 'progress') { mus.progress = ev; renderScanning(); } },
  });
  if (!final.ok) { mus.state = 'error'; mus.error = final; renderMusic(); return; }
  try { mus.catalog = await C.api('/api/catalog'); } catch (e) { mus.state = 'error'; mus.error = { code: 'internal', message: e.message }; renderMusic(); return; }
  mus.state = 'select';
  renderMusic();
  schedulePlan(0);
}

function schedulePlan(ms = 650) {
  clearTimeout(mus.planTimer);
  mus.planTimer = setTimeout(() => plan(), ms);
}

async function plan() {
  const d = device();
  if (!mus.catalog || !d || d.reachable === false || mus.state === 'applying') return;
  const token = ++mus.planToken;
  mus.planning = true;
  if (mus.state === 'plan-ready') mus.state = 'planning';
  else if (mus.state === 'select') mus.state = 'planning';
  renderSyncBar();
  const final = await C.runCli('sync-plan', {
    device: spec(), dest: mus.opts.dest || 'Music', layout: mus.opts.layout, covers: mus.opts.covers ? 'optimize' : 'keep',
    removeUnselected: mus.opts.remove, selection: selectionJson(mus.sel),
  }, { title: t('planJob'), quiet: true });
  if (token !== mus.planToken) return;
  mus.planning = false;
  if (final.ok) {
    try { mus.plan = await C.api('/api/plan'); } catch { mus.plan = final; }
    if (mus.state === 'planning') mus.state = 'plan-ready';
  } else {
    mus.plan = null;
    if (mus.state === 'planning') mus.state = 'select';
    mus.planError = final;
  }
  renderSyncBar();
  demoAfter();
}

function renderMusic() {
  if (!C || !$('music-body')) return;
  const body = $('music-body');
  body.replaceChildren();
  const s = mus.state;
  if (s === 'idle') { body.append(el('div', { class: 'card lib-loading' }, el('span', { class: 'spinner' }))); renderSyncBar(); return; }
  if (s === 'no-sources') { body.append(noSources()); renderSyncBar(); return; }
  if (s === 'applying') { body.append(applyingCard()); renderSyncBar(); return; }
  if (['done', 'partial', 'library-pending', 'cancelled'].includes(s)) { body.append(resultCard()); renderSyncBar(); return; }
  body.append(sourcesCard());
  if (s === 'scanning') body.append(scanningCard());
  else if (s === 'error') body.append(messageCard(ICONS.warn, C.errorMessage(mus.error), '', el('button', { class: 'btn', text: t('retry'), onclick: () => loadMusic({ rescan: true }) })));
  else if (mus.catalog) body.append(selectCard(), optionsCard());
  renderSyncBar();
}

function dropZone(node) {
  node.addEventListener('dragover', (e) => {
    if (!e.dataTransfer?.types.includes('Files')) return;
    e.preventDefault();
    node.classList.add('dragging');
  });
  node.addEventListener('dragleave', (e) => { if (!node.contains(e.relatedTarget)) node.classList.remove('dragging'); });
  node.addEventListener('drop', (e) => {
    e.preventDefault();
    node.classList.remove('dragging');
    dropMusic(e.dataTransfer);
  });
}

function noSources() {
  const hero = el('div', { class: 'music-hero' },
    el('div', { class: 'music-hero-icon' }, icon(ICONS.note)),
    el('h2', { text: t('musicNoSourcesTitle') }),
    el('p', { class: 'muted', text: t('musicNoSourcesBody') }),
    el('button', { class: 'btn primary big', text: t('musicChooseFolder'), onclick: () => pickFolder() }),
    el('div', { class: 'music-drop' }, el('span', { text: t('musicDrop') })),
    el('div', { class: 'drop-hint', text: t('musicDropHere') }));
  dropZone(hero);
  return hero;
}

function sourcesCard() {
  const chips = el('div', { class: 'chips' });
  for (const s of mus.sources ?? []) {
    // the folder's name (the whole path is in the tooltip)
    const name = s.temp ? t('musicDropped') : s.path.replace(/[\\/]+$/, '').split(/[\\/]/).pop() || s.path;
    chips.append(el('span', { class: `chip${s.exists ? '' : ' missing'}`, title: s.path },
      icon(ICONS.folder, 'i chip-i'), el('span', { class: 'chip-t', text: name }),
      s.exists ? null : el('span', { class: 'chip-tag', text: t('musicFolderMissing') }),
      el('button', { class: 'chip-x', 'aria-label': t('musicRemoveFolder'), title: t('musicRemoveFolder'), onclick: () => removeSource(s.path) }, icon(ICONS.close))));
  }
  const cat = mus.catalog;
  const line = cat ? t('musicLibraryLine', {
    songs: fmtCount(cat.counts?.tracks ?? 0), albums: fmtCount(cat.counts?.albums ?? 0),
    size: fmtBytes(cat.tracks.reduce((x, tr) => x + (tr.size || 0), 0)),
  }) : '';
  const card = el('div', { class: 'card sources' },
    el('div', { class: 'sources-head' },
      el('div', { class: 'sources-icon' }, icon(ICONS.note)),
      el('div', { class: 'grow' }, el('h3', { text: t('library') }), el('p', { class: 'muted small-text', text: line })),
      el('button', { class: 'btn link', text: t('musicRescan'), disabled: mus.state === 'scanning', onclick: () => loadMusic({ rescan: true }) }),
      el('button', { class: 'btn', text: t('musicAddFolder'), onclick: () => pickFolder() })),
    chips,
    el('div', { class: 'drop-hint', text: t('musicDropHere') }));
  dropZone(card);
  return card;
}

function scanningCard() {
  const card = el('div', { class: 'card lib-loading', id: 'music-scanning' });
  fillScanning(card);
  return card;
}
function renderScanning() { const c = $('music-scanning'); if (c) fillScanning(c); }
function fillScanning(card) {
  const p = mus.progress ?? {};
  let text = t('musicScanningList', { n: fmtCount(p.done ?? 0) });
  let frac = null;
  if (p.op === 'tags' && p.total > 0) { text = t('musicScanning', { done: fmtCount(p.done), total: fmtCount(p.total) }); frac = p.done / p.total; }
  if (mus.dropping) text = t('musicCopyingDrop', { n: mus.dropping });
  card.replaceChildren(el('div', { class: 'lib-loading-art' }, el('span'), el('span'), el('span')), el('p', { class: 'lib-loading-text', text }),
    el('div', { class: `step-bar${frac === null ? ' wait' : ''}` }, el('span', { style: frac === null ? '' : `width:${(frac * 100).toFixed(1)}%` })));
}

// The iTunes "Sync Music" box: everything, or chosen artists, albums and genres.
function selectCard() {
  const cat = mus.catalog;
  const sel = mus.sel;
  const changed = () => { saveSel(); schedulePlan(); };
  const radio = (all, label) => el('label', { class: 'radio' },
    el('input', { type: 'radio', name: 'sync-mode', checked: sel.all === all, onchange: () => { sel.all = all; changed(); renderMusic(); } }),
    el('span', { text: label }));
  const card = el('div', { class: 'card sync-card' },
    el('div', { class: 'sync-head' },
      el('label', { class: 'check big' }, el('input', { type: 'checkbox', checked: true, disabled: true }), el('span', { text: t('syncTitle') })),
      el('span', { class: 'muted small-text', text: t('musicLibraryLine', {
        songs: fmtCount(cat.counts.tracks), albums: fmtCount(cat.counts.albums), size: fmtBytes(cat.tracks.reduce((x, tr) => x + (tr.size || 0), 0)),
      }) })),
    el('div', { class: 'radios' }, radio(true, t('syncEntire')), radio(false, t('syncSelected'))));
  if (!sel.all) {
    const filter = el('input', { type: 'search', class: 'filter', placeholder: t('syncFilter'), value: mus.filter });
    filter.addEventListener('input', () => { mus.filter = filter.value; drawBoxes(); });
    const boxes = el('div', { class: 'pick-boxes' });
    const drawBoxes = () => {
      boxes.replaceChildren(
        pickBox(t('artists'), cat.artists.map((a) => ({ key: a.name.toLowerCase(), label: a.name, count: a.tracks })), sel.artists, changed),
        pickBox(t('albums'), cat.albums.map((a) => ({ key: `${a.artist.toLowerCase()}\u0000${a.title.toLowerCase()}`, label: a.title, sub: a.artist, count: a.tracks.length, art: a.art })), sel.albums, changed),
        pickBox(t('genres'), cat.genres.map((g) => ({ key: g.name.toLowerCase(), label: g.name, count: g.tracks })), sel.genres, changed));
    };
    drawBoxes();
    card.append(el('div', { class: 'filter-row' }, filter), boxes);
  }
  return card;
}

function pickBox(title, items, set, changed) {
  const shown = items.filter((i) => matches(mus.filter, i.label, i.sub ?? ''));
  const n = items.filter((i) => set.has(i.key)).length;
  const list = el('ul', { class: 'pick-list' });
  for (const it of shown) {
    const cb = el('input', { type: 'checkbox', checked: set.has(it.key) });
    cb.addEventListener('change', () => {
      if (cb.checked) set.add(it.key); else set.delete(it.key);
      list.closest('.pick-box').querySelector('.pick-count').textContent = n === 0 && !set.size ? '' : t('selCount', { n: items.filter((i) => set.has(i.key)).length });
      changed();
    });
    list.append(el('li', {}, el('label', { class: 'pick' }, cb,
      el('span', { class: 'pick-t' }, el('span', { text: it.label }), it.sub ? el('span', { class: 'pick-sub', text: it.sub }) : null),
      el('span', { class: 'pick-n', text: String(it.count) }))));
  }
  return el('div', { class: 'pick-box' },
    el('div', { class: 'pick-head' }, el('span', { text: title }), el('span', { class: 'pick-count', text: n ? t('selCount', { n }) : '' })),
    list);
}

function optionsCard() {
  const o = mus.opts;
  const changed = () => { saveSel(); schedulePlan(); };
  const check = (key, label, hint, sel = false) => el('label', { class: 'opt' },
    el('input', {
      type: 'checkbox', checked: sel ? mus.sel.books : o[key],
      onchange: (e) => { if (sel) mus.sel.books = e.target.checked; else o[key] = e.target.checked; changed(); },
    }),
    el('span', {}, el('span', { text: label }), hint ? el('span', { class: 'opt-hint', text: hint }) : null));
  const dest = el('input', { type: 'text', value: o.dest, spellcheck: 'false', class: 'dest' });
  dest.addEventListener('change', () => { o.dest = dest.value.trim().replace(/^\/+|\/+$/g, '') || 'Music'; dest.value = o.dest; changed(); });
  const layout = el('select', { class: 'layout' },
    el('option', { value: 'artist-album', text: t('layoutArtistAlbum'), selected: o.layout === 'artist-album' }),
    el('option', { value: 'keep', text: t('layoutKeep'), selected: o.layout === 'keep' }));
  layout.addEventListener('change', () => { o.layout = layout.value; changed(); });
  return el('div', { class: 'card options-card' },
    el('div', { class: 'opts' },
      check('books', t('optBooks'), '', true),
      check('lyrics', t('optLyrics')),
      check('covers', t('optCovers'), t('optCoversHint')),
      check('remove', t('optRemove'), t('optRemoveHint'))),
    el('div', { class: 'opts-fields' },
      el('label', { class: 'field' }, el('span', { text: t('optDest') }), dest),
      el('label', { class: 'field' }, el('span', { text: t('optLayout') }), layout)));
}

// The bottom bar: iTunes' capacity bar, the plan in one line, and Apply.
function renderSyncBar() {
  const bar = $('sync-bar');
  if (!bar) return;
  const show = ['select', 'planning', 'plan-ready'].includes(mus.state) && C.appState.tab === 'music' && !!mus.catalog;
  bar.hidden = !show;
  if (!show) return;
  bar.replaceChildren();
  const sd = C.appState.status?.sd;
  const p = mus.plan;
  const total = sd?.sd_total || (p ? p.sd_free * 2 : 0);
  const free = p ? p.sd_free : sd?.sd_free ?? 0;
  const used = total ? total - free : 0;
  const add = p ? Math.max(0, p.bytes_add - (p.bytes_remove ?? 0)) : 0;
  const cap = el('div', { class: `capacity sync-cap${p && !p.fits ? ' over' : ''}` });
  const parts = [['used', used], ['new', Math.min(add, free)], ['free', p ? planFreeAfter(p) : free]];
  for (const [cls, v] of parts) {
    const span = el('span', { class: cls });
    cap.append(span);
    requestAnimationFrame(() => { span.style.flexBasis = `${total ? (100 * v) / total : 0}%`; });
  }
  const legend = el('div', { class: 'legend' },
    el('span', {}, el('i', { class: 'lg-used' }), t('used'), el('span', { class: 'v', text: fmtBytes(used) })),
    el('span', {}, el('i', { class: 'lg-new' }), t('capNew'), el('span', { class: 'v', text: fmtBytes(add) })),
    el('span', {}, el('i', { class: 'lg-free' }), t('capFree'), el('span', { class: 'v', text: fmtBytes(p ? planFreeAfter(p) : free) })));
  let text;
  let cls = 'sync-text';
  if (mus.planning || mus.state === 'planning') text = el('span', {}, el('span', { class: 'spinner' }), ` ${t('planning')}`);
  else if (p && !p.fits) { text = t('notEnoughSpace'); cls += ' fail'; }
  else if (p && p.add + p.update + p.remove === 0) text = t('planNothing');
  else if (p) text = planSummary(p, t, lang());
  else text = mus.planError ? C.errorMessage(mus.planError) : '';
  const apply = el('button', {
    class: 'btn primary apply', text: t('apply'),
    disabled: !p || !p.fits || mus.planning || p.add + p.update + p.remove === 0 || device()?.reachable === false,
    onclick: () => applySync(),
  });
  bar.append(el('div', { class: 'sync-bar-cap' }, cap, legend),
    el('div', { class: 'sync-bar-row' }, typeof text === 'string' ? el('span', { class: cls, text }) : el('span', { class: cls }, text), apply));
}

async function applySync(retry = false) {
  const p = mus.plan;
  if (!p) return;
  if (p.remove > 0 && !retry) {
    const r = await C.sheet({ title: t('syncConfirmRemoveTitle', { n: p.remove }), body: t('syncConfirmRemoveBody'), okText: t('deleteAction'), danger: true });
    if (!r.ok) return;
  }
  mus.state = 'applying';
  mus.progress = { files_total: p.add + p.update, files_done: 0, done: 0, total: p.bytes_add };
  mus.failed = [];
  mus.final = null;
  renderMusic();
  const final = await C.runCli('sync-apply', {
    device: spec(), covers: mus.opts.covers ? 'optimize' : 'keep', lyrics: mus.opts.lyrics, then: 'rescan', yes: true,
  }, {
    title: t('syncJob'),
    onStarted: (job) => { mus.job = job; },
    onEvent: (ev) => {
      if (ev.event === 'progress') {
        if (ev.op === 'sync') Object.assign(mus.progress, { done: ev.done, total: ev.total, rate: ev.rate, files_done: ev.files_done, files_total: ev.files_total });
        else if (ev.op === 'put') Object.assign(mus.progress, { file: ev.path, fileDone: ev.done, fileTotal: ev.total, phase: 'put' });
        else if (ev.op === 'prepare') Object.assign(mus.progress, { file: ev.path, phase: 'prepare', fileDone: 0, fileTotal: 0 });
        else if (ev.op === 'remove') mus.progress.phase = 'remove';
        else if (ev.op === 'library') mus.progress.phase = 'library';
        renderApplying();
      } else if (ev.event === 'warn' && ev.code === 'failed') mus.failed.push(ev);
    },
  });
  mus.job = null;
  mus.final = final;
  const outcome = syncOutcome(final);
  mus.state = outcome === 'error' ? 'error' : outcome;
  if (outcome === 'error') mus.error = final;
  if (final.ok || outcome === 'cancelled') { lib.index = null; mus.plan = null; }
  renderMusic();
}

function applyingCard() {
  const card = el('div', { class: 'card applying', id: 'applying' });
  fillApplying(card);
  return card;
}
function renderApplying() { const c = $('applying'); if (c) fillApplying(c); }
function fillApplying(card) {
  const p = mus.progress;
  const frac = p.total ? Math.min(1, p.done / p.total) : 0;
  const fileFrac = p.fileTotal ? Math.min(1, p.fileDone / p.fileTotal) : null;
  const name = (p.file ?? '').split('/').pop().replace(/\.[^.]+$/, '');
  let sub = '';
  if (p.phase === 'prepare') sub = t('syncPreparing', { name });
  else if (p.phase === 'remove') sub = t('syncRemoving');
  else if (p.phase === 'library') sub = t('syncLibrary');
  else sub = name;
  const left = fmtLeft(secondsLeft(p.done, p.total, p.rate), t);
  const rate = p.rate ? t('rateText', { size: fmtBytes(p.rate) }) : '';
  card.replaceChildren(
    el('div', { class: 'applying-head' },
      el('div', { class: 'applying-icon' }, icon(ICONS.sync)),
      el('div', { class: 'grow' },
        el('h2', { text: t('syncCopying', { done: fmtCount(Math.min((p.files_done ?? 0) + 1, p.files_total ?? 0)), total: fmtCount(p.files_total ?? 0) }) }),
        el('p', { class: 'muted', text: [`${fmtBytes(p.done)} / ${fmtBytes(p.total)}`, rate, left].filter(Boolean).join(' · ') })),
      el('button', { class: 'btn', text: t('cancel'), onclick: () => cancelSync() })),
    el('div', { class: 'step-bar big' }, el('span', { style: `width:${(frac * 100).toFixed(1)}%` })),
    el('div', { class: 'applying-file' },
      el('span', { class: 'applying-name', text: sub }),
      el('div', { class: `step-bar small${fileFrac === null ? ' wait' : ''}` }, el('span', { style: fileFrac === null ? '' : `width:${(fileFrac * 100).toFixed(1)}%` }))),
    ...(mus.failed.length ? [el('p', { class: 'fail-text small-text', text: t('syncPartial', { n: mus.failed.length }) })] : []));
}

function cancelSync() {
  if (mus.job?.id) C.api(`/api/jobs/${mus.job.id}/cancel`, { method: 'POST', body: {} }).catch(() => {});
}

function resultCard() {
  const f = mus.final ?? {};
  const s = mus.state;
  const ok = s === 'done' || s === 'library-pending';
  const card = el('div', { class: `card result ${ok ? 'ok' : s === 'partial' ? 'partial' : 'stopped'}` });
  const title = s === 'partial' ? t('syncPartial', { n: f.failed }) : s === 'cancelled' ? t('syncCancelled') : t('syncDone');
  card.append(el('div', { class: 'result-icon' }, icon(ok ? ICONS.check : s === 'partial' ? ICONS.warn : ICONS.info)),
    el('h2', { text: title }));
  if (s !== 'cancelled') {
    card.append(el('p', { class: 'muted', text: t('syncDoneBody', { written: fmtCount(f.written ?? 0), size: fmtBytes(f.bytes ?? 0), skipped: fmtCount(f.skipped ?? 0), removed: fmtCount(f.removed ?? 0) }) }));
  }
  if (s === 'partial') {
    const ul = el('ul', { class: 'fail-list' });
    const failed = (f.items ?? []).filter((i) => i.result === 'failed');
    for (const i of failed.slice(0, 12)) ul.append(el('li', {}, el('span', { text: i.dest.split('/').pop() }), el('span', { class: 'muted', text: i.error ?? '' })));
    card.append(el('p', { class: 'muted', text: t('syncPartialBody') }), ul);
  }
  if (s === 'library-pending' || (s === 'partial' && f.library && !f.library.ok)) {
    card.append(el('div', { class: 'pending' }, icon(ICONS.info, 'i pending-i'),
      el('div', {}, el('p', { text: t('libraryPending') }),
        el('div', { class: 'howto' }, el('span', { class: 'howto-step', text: t('howtoSettings') }), el('span', { text: '›' }), el('span', { class: 'howto-step', text: t('howtoUpdate') })))));
  }
  const actions = el('div', { class: 'result-actions' });
  if (s === 'partial') actions.append(el('button', { class: 'btn primary', text: t('retryFailed'), onclick: () => retryFailed() }));
  actions.append(el('button', { class: `btn${s === 'partial' ? '' : ' primary'}`, text: t('syncAgain'), onclick: () => { mus.state = 'select'; mus.final = null; renderMusic(); schedulePlan(0); } }));
  card.append(actions);
  return card;
}

async function retryFailed() {
  mus.state = 'select';
  renderMusic();
  await plan();
  if (mus.plan && mus.plan.fits && mus.plan.add + mus.plan.update > 0) applySync(true);
}

async function pickFolder() {
  try {
    const r = await C.api('/api/pick-folder', { method: 'POST', body: {} });
    mus.sources = r.sources;
    if (r.picked) { mus.catalog = null; await loadMusic({ rescan: true }); }
  } catch (e) { toast(e.message, 'fail'); }
}

async function removeSource(p) {
  try {
    const r = await C.api('/api/sources', { method: 'DELETE', body: { path: p } });
    mus.sources = r.sources;
    mus.catalog = null;
    mus.plan = null;
    await loadMusic({ rescan: mus.sources.length > 0 });
  } catch (e) { toast(e.message, 'fail'); }
}

// Files and folders dropped on the Music tab: copied into one "incoming" folder for this drop
// (the browser gives no paths), which then becomes a music source.
async function dropMusic(dt) {
  const entries = [...(dt?.items ?? [])].map((i) => i.webkitGetAsEntry?.()).filter(Boolean);
  const files = [];
  const walk = (entry, prefix) => new Promise((resolve) => {
    if (entry.isFile) { entry.file((f) => { files.push({ file: f, rel: prefix + f.name }); resolve(); }, () => resolve()); return; }
    if (!entry.isDirectory) { resolve(); return; }
    const reader = entry.createReader();
    const all = [];
    const more = () => reader.readEntries(async (batch) => {
      if (!batch.length) { for (const e of all) await walk(e, `${prefix}${entry.name}/`); resolve(); return; }
      all.push(...batch); more();
    }, () => resolve());
    more();
  });
  for (const e of entries) await walk(e, '');
  const audio = files.filter((f) => /\.(flac|mp3|m4a|m4b|aac|wav|aiff?|dsf|dff|ape|ogg|oga|opus|wma|mp2|lrc|jpe?g|png)$/i.test(f.file.name) && !f.rel.split('/').some((c) => c.startsWith('.')));
  if (!audio.length) return;
  const session = Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) => b.toString(16).padStart(2, '0')).join('');
  mus.state = 'scanning';
  mus.dropping = audio.length;
  renderMusic();
  try {
    for (const f of audio) {
      const q = new URLSearchParams({ session, rel: f.rel.normalize('NFC') });
      if (f.file.lastModified > 0) q.set('mtime', String(f.file.lastModified));
      await C.api(`/api/incoming?${q}`, { method: 'POST', raw: f.file });
    }
    const r = await C.api(`/api/incoming/done?session=${session}`, { method: 'POST', body: {} });
    mus.sources = r.sources;
  } catch (e) { toast(e.message, 'fail'); }
  mus.dropping = null;
  mus.catalog = null;
  await loadMusic({ rescan: true });
}

// ---- wiring -------------------------------------------------------------------------------------

export function initMedia(ctx) {
  C = ctx;
  for (const b of document.querySelectorAll('#lib-views button')) {
    b.addEventListener('click', () => setView(b.dataset.view));
  }
  for (const b of document.querySelectorAll('.side-lib')) {
    b.addEventListener('click', () => { setView(b.dataset.libview); if (C.appState.tab !== 'library') C.setTab('library'); });
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
    if (C.appState.tab !== 'library' || document.querySelector('dialog[open]') || e.target.closest?.('input, select')) return;
    if (e.key === 'Delete' && lib.selected.size) deleteTracks([...lib.selected]);
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') { e.preventDefault(); $('lib-search').focus(); }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'i' && lib.selected.size) { e.preventDefault(); getInfo([...lib.selected]); }
  });
  document.getElementById('content').addEventListener('scroll', closeMenu, { passive: true });
  window.addEventListener('resize', () => {
    if (C.appState.tab === 'library' && lib.view === 'albums' && lib.open !== null) {
      const grid = document.querySelector('.album-grid');
      if (grid) placeDetail(grid, true);
    }
  });
}

function setView(v) {
  lib.view = v;
  lib.selected.clear();
  closeMenu();
  C.saveLocation();
  renderLibrary();
}

export function mediaLocation() {
  return C.appState.tab === 'library' && lib.view !== 'albums' ? `/${lib.view}` : '';
}

export function readMediaLocation(rest) {
  if (['albums', 'artists', 'songs', 'books', 'playlists', 'recent'].includes(rest)) lib.view = rest;
}

// The selected HiBy changed (or was refreshed).
export function mediaDeviceChanged() {
  if (lib.demo && lib.loading) return; // a fixed demo picture
  lib.deviceId = null;
  lib.index = null;
  lib.state = 'idle';
  mus.plan = null;
}

export function mediaTabOpened(tab) {
  if (tab === 'library') { renderLibrary(); loadLibrary(); }
  if (tab === 'music') { renderMusic(); loadMusic(); }
  if (tab !== 'music') renderSyncBar();
}

// The app re-renders its page on every device refresh (every 5 s): the Library and Music tabs
// are only rebuilt when something they show changed, so animations, scroll positions, an open
// album and a half-typed filter stay as they are.
let mediaKey = '';
export function renderMedia() {
  const d = device();
  const key = JSON.stringify([lang(), d?.id, d?.reachable, C.appState.tab, !!C.appState.status]);
  if (key === mediaKey) return;
  mediaKey = key;
  renderLibrary();
  renderMusic();
}

// Demo mode only (?demo=NAME&...): fixed pictures of the Library and Music states, for
// screenshots and design review. The rest of the page is the real (mock) flow.
export async function demoMedia(name, params) {
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
  if (name.startsWith('music-')) {
    mus.demo = name;
    if (name === 'music-empty') {
      try { for (const s of (await C.api('/api/sources')).sources) await C.api('/api/sources', { method: 'DELETE', body: { path: s.path } }); } catch { /* demo */ }
      return 'music';
    }
    try { await C.api('/api/pick-folder', { method: 'POST', body: {} }); } catch { /* demo */ }
    if (params.get('selected') === '1') {
      mus.sel = { all: false, artists: new Set(['aurora fields', 'luna varga']), albums: new Set(), genres: new Set(['jazz']), books: true };
    }
    return 'music';
  }
  return null;
}

// Called by the demo once the real flow reached its state.
export function demoAfter() {
  if (lib.afterLoad && lib.state === 'ready') { const f = lib.afterLoad; lib.afterLoad = null; f(); }
  const d = mus.demo;
  if (!d || !mus.catalog) return false;
  const tracks = mus.catalog.tracks;
  const base = { files_total: 188, files_done: 41, done: 1838000000, total: 7898666830, rate: 11200000 };
  if (d === 'music-scanning') { mus.state = 'scanning'; mus.progress = { op: 'tags', done: 1240, total: 3900 }; }
  else if (d === 'music-applying') { mus.state = 'applying'; mus.progress = { ...base, file: tracks[7].path, fileDone: 21000000, fileTotal: 33000000, phase: 'put' }; mus.failed = []; }
  else if (d === 'music-partial') {
    mus.state = 'partial';
    mus.final = { ok: true, written: 185, skipped: 117, failed: 3, removed: 0, bytes: 7712000000, library: { action: 'rescan', ok: false, code: 'E_UNSUPPORTED' },
      items: [0, 1, 2].map((k) => ({ dest: `Music/${tracks[k * 9].artist}/${tracks[k * 9].album}/${tracks[k * 9].title}.flac`, result: 'failed', error: 'E_IO' })) };
  } else if (d === 'music-pending') {
    mus.state = 'library-pending';
    mus.final = { ok: true, written: 188, skipped: 117, failed: 0, removed: 2, bytes: 7898666830, library: { action: 'rescan', ok: false, code: 'E_UNSUPPORTED' }, items: [] };
  } else if (d === 'music-done') {
    mus.state = 'done';
    mus.final = { ok: true, written: 188, skipped: 117, failed: 0, removed: 0, bytes: 7898666830, library: { action: 'rescan', ok: true }, items: [] };
  } else if (d === 'music-full' && mus.plan) { mus.plan = { ...mus.plan, fits: false, sd_free: 1200000000 }; }
  else return false;
  mus.demo = null;
  renderMusic();
  return true;
}

export function mediaState() { return { lib, mus }; }
