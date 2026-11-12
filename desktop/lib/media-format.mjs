// Rules of the Library and Music tabs that do not need a page: text folding for search,
// times, sizes, the sync summary line, the virtual list window and how a sync ended. Shared by
// the browser (/media-format.js) and the tests.

// Case- and accent-insensitive text ("Beyoncé" matches "beyonce").
export function fold(s) {
  return String(s ?? '').normalize('NFD').replace(/\p{M}+/gu, '').toLowerCase();
}

// "The Beatles" sorts under B; empty names last (LibraryIndex.kt).
export function sortKey(s) {
  if (!s) return '￿';
  const l = fold(s);
  return l.startsWith('the ') && l.length > 4 ? l.slice(4) : l;
}

export function compareNames(a, b) {
  const x = sortKey(a); const y = sortKey(b);
  return x < y ? -1 : x > y ? 1 : 0;
}

// Groups tracks into albums, artists, genres and audiobooks with the rules of byos-cli's
// LibraryIndex.kt (album artist else artist, "The " ignored, tracks by disc and number), so
// the page can drop deleted songs without reading the HiBy again. Returns the index.json form.
export function groupIndex(tracksIn, { source = 'snapshot', revision = null, time = Math.floor(Date.now() / 1000), playlists = [] } = {}) {
  const tracks = [...tracksIn].sort((a, b) => compareNames(a.title, b.title) || (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  const byAlbum = new Map();
  tracks.forEach((t, i) => {
    if (t.book) return;
    const k = `${fold(t.albumArtist || t.artist)}\u0000${fold(t.album)}`;
    if (!byAlbum.has(k)) byAlbum.set(k, []);
    byAlbum.get(k).push(i);
  });
  const num = (n) => (n > 0 ? n : Number.MAX_SAFE_INTEGER);
  const albums = [...byAlbum.values()].map((list) => {
    list.sort((x, y) => tracks[x].disc - tracks[y].disc || num(tracks[x].track) - num(tracks[y].track) || (tracks[x].path < tracks[y].path ? -1 : 1));
    const f = tracks[list[0]];
    const genres = new Map();
    for (const i of list) if (tracks[i].genre) genres.set(tracks[i].genre, (genres.get(tracks[i].genre) ?? 0) + 1);
    const genre = [...genres.entries()].sort((a, b) => b[1] - a[1])[0]?.[0] ?? '';
    return {
      title: f.album, artist: f.albumArtist || f.artist, year: Math.max(...list.map((i) => tracks[i].year || 0)), genre,
      tracks: list, art: tracks[list.find((i) => tracks[i].art) ?? list[0]].path,
    };
  }).sort((a, b) => compareNames(a.title, b.title) || compareNames(a.artist, b.artist));
  const byArtist = new Map();
  albums.forEach((a, i) => { const k = fold(a.artist); if (!byArtist.has(k)) byArtist.set(k, []); byArtist.get(k).push(i); });
  const artists = [...byArtist.values()].map((list) => {
    list.sort((x, y) => num(albums[x].year) - num(albums[y].year) || compareNames(albums[x].title, albums[y].title));
    return { name: albums[list[0]].artist, albums: list, tracks: list.reduce((s, i) => s + albums[i].tracks.length, 0) };
  }).sort((a, b) => compareNames(a.name, b.name));
  const genres = new Map();
  for (const t of tracks) {
    if (t.book || !t.genre) continue;
    const k = fold(t.genre);
    genres.set(k, { name: genres.get(k)?.name ?? t.genre, tracks: (genres.get(k)?.tracks ?? 0) + 1 });
  }
  const byBook = new Map();
  tracks.forEach((t, i) => {
    if (!t.book) return;
    const k = t.path.toLowerCase().endsWith('.m4b') ? t.path : t.path.slice(0, Math.max(0, t.path.lastIndexOf('/'))) || t.path;
    if (!byBook.has(k)) byBook.set(k, []);
    byBook.get(k).push(i);
  });
  const books = [...byBook.entries()].map(([k, list]) => {
    list.sort((x, y) => tracks[x].disc - tracks[y].disc || tracks[x].track - tracks[y].track || (tracks[x].path < tracks[y].path ? -1 : 1));
    const f = tracks[list[0]];
    return {
      name: f.album || (k === f.path ? f.title : k.slice(k.lastIndexOf('/') + 1)), path: k, tracks: list,
      duration: list.reduce((s, i) => s + (tracks[i].duration || 0), 0), size: list.reduce((s, i) => s + (tracks[i].size || 0), 0),
    };
  }).sort((a, b) => compareNames(a.name, b.name));
  const sortedPlaylists = [...playlists].sort((a, b) => compareNames(a.name, b.name));
  return {
    format: 1, source, revision, time,
    counts: {
      tracks: tracks.filter((t) => !t.book).length, albums: albums.length, artists: artists.length, genres: genres.size,
      books: books.length, playlists: sortedPlaylists.length,
    },
    tracks, albums, artists, genres: [...genres.values()].sort((a, b) => compareNames(a.name, b.name)), books, playlists: sortedPlaylists,
  };
}

// Every word of the query appears in one of the fields.
export function matches(query, ...fields) {
  const words = fold(query).split(/\s+/).filter(Boolean);
  if (!words.length) return true;
  const hay = fields.map(fold).join('\u0001');
  return words.every((w) => hay.includes(w));
}

// 225000 -> "3:45"; 3723000 -> "1:02:03".
export function fmtDuration(ms) {
  if (!ms || ms < 0) return '';
  const s = Math.round(ms / 1000);
  const h = Math.floor(s / 3600); const m = Math.floor((s % 3600) / 60); const r = s % 60;
  return h ? `${h}:${String(m).padStart(2, '0')}:${String(r).padStart(2, '0')}` : `${m}:${String(r).padStart(2, '0')}`;
}

// Decimal units, as the HiBy's Summary page and Windows Explorer's "GB" on cards.
export function fmtBytes(n, lang = 'en') {
  if (n === undefined || n === null || Number.isNaN(n)) return '';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  let v = Math.abs(n);
  while (v >= 1000 && i < units.length - 1) { v /= 1000; i++; }
  const s = new Intl.NumberFormat(lang, { maximumFractionDigits: v < 10 && i > 0 ? 1 : 0 }).format(v);
  return `${n < 0 ? '-' : ''}${s} ${units[i]}`;
}

export function fmtCount(n, lang = 'en') {
  return new Intl.NumberFormat(lang).format(n ?? 0);
}

// The sync summary under the Music tab, as iTunes shows it above its capacity bar:
// "Add 312 songs (2.1 GB) · update 4 · remove 0 · 58 GB free".
export function planSummary(plan, t, lang = 'en') {
  if (!plan) return '';
  const parts = [t('planAdd', { n: fmtCount(plan.add, lang), size: fmtBytes(plan.bytes_add, lang) })];
  parts.push(t('planUpdate', { n: fmtCount(plan.update, lang) }));
  parts.push(t('planRemove', { n: fmtCount(plan.remove, lang) }));
  parts.push(t('planFree', { size: fmtBytes(plan.sd_free, lang) }));
  return parts.join(' · ');
}

// Free space after the sync (never below zero), for the capacity bar.
export function planFreeAfter(plan) {
  if (!plan) return 0;
  return Math.max(0, plan.sd_free - plan.bytes_add + (plan.bytes_remove ?? 0));
}

// The rows to draw for a list of n rows of rowH pixels, scrolled to scrollTop in a box viewH
// high, with overscan extra rows on each side.
export function virtualWindow(scrollTop, viewH, rowH, n, overscan = 8) {
  const first = Math.max(0, Math.floor(scrollTop / rowH) - overscan);
  const last = Math.min(n, Math.ceil((scrollTop + viewH) / rowH) + overscan);
  return { start: first, end: Math.max(first, last), top: first * rowH, height: n * rowH };
}

// "2 min ago" style age of the library index.
export function age(sec, nowSec, t) {
  const d = Math.max(0, Math.round(nowSec - sec));
  if (d < 60) return t('ageNow');
  if (d < 3600) return t('ageMin', { n: Math.floor(d / 60) });
  if (d < 86400) return t('ageHour', { n: Math.floor(d / 3600) });
  return t('ageDay', { n: Math.floor(d / 86400) });
}

// Seconds left at the current rate, or null.
export function secondsLeft(done, total, rate) {
  if (!rate || rate <= 0 || !total || done >= total) return null;
  return Math.ceil((total - done) / rate);
}

export function fmtLeft(sec, t) {
  if (sec === null || sec === undefined) return '';
  if (sec < 60) return t('leftSec', { n: Math.max(1, sec) });
  if (sec < 3600) return t('leftMin', { n: Math.ceil(sec / 60) });
  return t('leftHour', { h: Math.floor(sec / 3600), m: Math.ceil((sec % 3600) / 60) });
}

// How a sync-apply run ended, for the Music tab: done, partial (some files failed),
// library-pending (copied, but the HiBy's library is not updated yet), cancelled or error.
export function syncOutcome(final) {
  if (!final) return 'error';
  if (!final.ok) return final.code === 'cancelled' ? 'cancelled' : 'error';
  if ((final.failed ?? 0) > 0) return 'partial';
  if (final.library && final.library.action !== 'none' && !final.library.ok) return 'library-pending';
  return 'done';
}

// What the Library tab shows after lib-index or lib-scan.
export function libraryState(final, index) {
  if (!final) return 'loading';
  if (!final.ok) return final.hint === 'scan' ? 'source-unavailable' : 'error';
  if (!index || !index.tracks || index.tracks.length === 0) return 'empty';
  return 'ready';
}

// A rough time for a card scan: about 8 files a second over USB, a file per 30 MB on the card.
export function scanEstimate(usedBytes) {
  const files = Math.max(1, Math.round((usedBytes ?? 0) / 30e6));
  return { files, minutes: Math.max(1, Math.round(files / 8 / 60)) };
}

// "FLAC 24/96", "MP3 320 kbps", "ALAC 16/44.1".
export function quality(t) {
  if (!t) return '';
  const f = t.format ?? '';
  if (t.lossless && t.sr) {
    const khz = t.sr % 1000 === 0 ? t.sr / 1000 : (t.sr / 1000).toFixed(1);
    return t.bits ? `${f} ${t.bits}/${khz}` : `${f} ${khz} kHz`;
  }
  return t.kbps ? `${f} ${t.kbps} kbps` : f;
}

// The selection sent to sync-plan (BYOS_CLI.md, SEL.json).
export function selectionJson(sel) {
  if (sel.all) return { all: true, books: !!sel.books };
  return {
    all: false,
    artists: [...sel.artists],
    albums: [...sel.albums].map((k) => { const [artist, title] = k.split('\u0000'); return { artist, title }; }),
    genres: [...sel.genres],
    files: [...(sel.files ?? [])],
    books: !!sel.books,
  };
}

// Readable text on a cover colour: dark text on light covers.
export function textOn(hex) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex ?? '');
  if (!m) return 'light';
  const [r, g, b] = m.slice(1).map((x) => parseInt(x, 16) / 255).map((c) => (c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b > 0.4 ? 'dark' : 'light';
}
