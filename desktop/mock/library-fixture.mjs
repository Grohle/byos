// Made-up music for the demo mode: about 40 albums, 400 songs and 6 audiobooks on the fake
// HiBy, a music folder "on this computer", and cover pictures drawn as PNG files with
// node:zlib. Every name here is invented. Deterministic: the same data on every run.

import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { groupIndex } from '../lib/media-format.mjs';

// The grouping of byos-cli's LibraryIndex.kt (shared with the page).
function group(tracks, source, playlists) {
  return groupIndex(tracks, {
    source, revision: source === 'snapshot' ? 'ab'.repeat(32) : null, time: Math.floor(Date.now() / 1000) - 120, playlists,
  });
}

// ---- a tiny deterministic random source ---------------------------------------------------

function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

const ARTISTS = [
  ['Aurora Fields', 'Pop'], ['The Midnight Static', 'Rock'], ['Luna Varga', 'Jazz'], ['Northbound Trains', 'Indie'],
  ['Coral & Ash', 'Electronic'], ['Matteo Brandão', 'Bossa Nova'], ['The Paper Lanterns', 'Folk'], ['Saffron Kite', 'Pop'],
  ['Elena Okafor', 'Soul'], ['Glass Harbour', 'Ambient'], ['Rue Monsoon', 'Electronic'], ['The Velvet Orchard', 'Rock'],
  ['Kazu Mori Trio', 'Jazz'], ['Isla Ferro', 'Classical'], ['Brass Meridian', 'Funk'], ['Hollow Pines', 'Folk'],
  ['Neon Atlas', 'Electronic'], ['Clémence Duval', 'Chanson'],
];
const WORDS = ['Blue', 'Hours', 'Summer', 'Echo', 'Paper', 'Moon', 'River', 'Static', 'Golden', 'Night', 'Glass', 'Garden',
  'Signal', 'Northern', 'Lights', 'Velvet', 'Wild', 'Quiet', 'Fire', 'Harbour', 'Satellite', 'Morning', 'Silver', 'Ghost',
  'Island', 'Neon', 'Tides', 'Winter', 'Atlas', 'Dream', 'Canyon', 'Ember', 'Orbit', 'Rain', 'Lantern', 'Coast'];
const TITLE_WORDS = ['Waiting', 'Under', 'Lights', 'Slow', 'Hearts', 'Running', 'Home', 'Again', 'Falling', 'Strangers',
  'Electric', 'Holding', 'Shadows', 'Distance', 'Weather', 'Sparks', 'Closer', 'Midnight', 'Letters', 'Dancing', 'Gravity',
  'Paper Boats', 'Stay', 'Wonder', 'Fever', 'Halo', 'Mirrors', 'Velocity', 'Satellites', 'Driftwood', 'Señorita', 'Café',
  'Nostalgia', 'Tidal', 'Cinnamon', 'Fireflies', 'Polaroid', 'Afterglow', 'Lullaby', 'Avalanche'];
const BOOKS = [
  ['The Lighthouse Keeper', 'M. A. Holloway', 12], ['A Short History of Rivers', 'Dana Whitfield', 8],
  ['Midnight in Valparaíso', 'Inés Carrera', 15], ['The Quiet Engineer', 'Theo Lindqvist', 10],
  ['Letters from the Coast', 'Amara Nwosu', 6], ['Stars Over the Desert', 'R. J. Okonkwo', 9],
];
const PALETTE = [['#ff5f6d', '#ffc371'], ['#2193b0', '#6dd5ed'], ['#cc2b5e', '#753a88'], ['#42275a', '#734b6d'],
  ['#de6262', '#ffb88c'], ['#06beb6', '#48b1bf'], ['#eb3349', '#f45c43'], ['#dd5e89', '#f7bb97'], ['#56ab2f', '#a8e063'],
  ['#614385', '#516395'], ['#4776e6', '#8e54e9'], ['#1d976c', '#93f9b9'], ['#e65c00', '#f9d423'], ['#2b5876', '#4e4376'],
  ['#ff4e50', '#f9d423'], ['#b24592', '#f15f79'], ['#0f2027', '#2c5364'], ['#c94b4b', '#4b134f'], ['#00b09b', '#96c93d'],
  ['#fc4a1a', '#f7b733'], ['#3a1c71', '#ffaf7b'], ['#16222a', '#3a6073'], ['#ee0979', '#ff6a00'], ['#8e2de2', '#4a00e0']];

const cap = (s) => s.charAt(0).toUpperCase() + s.slice(1);
const pad = (n) => String(n).padStart(2, '0');

function makeAlbums(seed, count) {
  const r = rng(seed);
  const pick = (a) => a[Math.floor(r() * a.length)];
  const albums = [];
  const used = new Set();
  for (let i = 0; i < count; i++) {
    const [artist, genre] = ARTISTS[i % ARTISTS.length];
    let title;
    do { title = r() < 0.4 ? `${pick(WORDS)} ${pick(WORDS)}` : r() < 0.5 ? `The ${pick(WORDS)} ${pick(WORDS)}` : pick(WORDS); } while (used.has(title));
    used.add(title);
    const n = 7 + Math.floor(r() * 7);
    const year = 1998 + Math.floor(r() * 27);
    const lossless = r() < 0.75;
    const tracks = [];
    for (let k = 1; k <= n; k++) {
      const name = r() < 0.5 ? `${pick(TITLE_WORDS)} ${pick(TITLE_WORDS).toLowerCase()}` : pick(TITLE_WORDS);
      const dur = 150000 + Math.floor(r() * 210000);
      tracks.push({
        title: cap(name), track: k, disc: 0, duration: dur,
        sr: lossless ? (r() < 0.3 ? 96000 : 44100) : 44100, bits: lossless ? (r() < 0.3 ? 24 : 16) : 0,
        kbps: lossless ? 0 : 320, lossless,
        size: lossless ? Math.round(dur * (r() < 0.3 ? 420 : 110)) : Math.round(dur * 40),
      });
    }
    albums.push({ artist, title, genre, year, tracks, palette: PALETTE[(i * 7 + seed) % PALETTE.length], style: i % 5, ext: lossless ? 'flac' : 'mp3' });
  }
  return albums;
}

function clean(s) {
  return s.replace(/[\\/:*?"<>|]/g, '_').replace(/[. ]+$/, '');
}

// The fake HiBy's library, as lib-index writes it (docs/BYOS_CLI.md).
export function hibyIndex({ source = 'snapshot', deleted = [] } = {}) {
  const albums = makeAlbums(7, 40);
  const tracks = [];
  const t0 = 1704067200;
  albums.forEach((a, ai) => {
    for (const t of a.tracks) {
      const path = `Music/${clean(a.artist)}/${clean(a.title)}/${pad(t.track)} ${clean(t.title)}.${a.ext}`;
      if (deleted.includes(path)) continue;
      tracks.push({
        id: tracks.length + 1, path, title: t.title, artist: a.artist, albumArtist: '', album: a.title, genre: a.genre,
        year: a.year, track: t.track, disc: 0, duration: t.duration, format: a.ext.toUpperCase(), sr: t.sr, bits: t.bits,
        kbps: t.kbps, lossless: t.lossless, size: t.size, mtime: t0 + ai * 86400, ctime: t0 + ai * 86400, art: true, book: false,
      });
    }
  });
  BOOKS.forEach(([title, author, parts], bi) => {
    for (let k = 1; k <= parts; k++) {
      const path = `Audiobooks/${clean(title)}/${pad(k)} Chapter ${k}.mp3`;
      if (deleted.includes(path)) continue;
      tracks.push({
        id: tracks.length + 1, path, title: `Chapter ${k}`, artist: author, albumArtist: '', album: title, genre: 'Audiobook',
        year: 2015 + bi, track: k, disc: 0, duration: 1500000 + k * 60000, format: 'MP3', sr: 44100, bits: 0, kbps: 64,
        lossless: false, size: 12000000 + k * 400000, mtime: t0, ctime: t0, art: true, book: true,
      });
    }
  });
  return group(tracks, source, [
    { path: 'Playlists/Road trip.m3u8', name: 'Road trip' },
    { path: 'Playlists/Late night.m3u8', name: 'Late night' },
    { path: 'Playlists/Running.m3u8', name: 'Running' },
  ]);
}

// Music "on this computer" for the Music tab: some of it is on the HiBy already.
export function localCatalog(roots) {
  const albums = [...makeAlbums(7, 40).slice(0, 12), ...makeAlbums(31, 18)];
  const root = roots[0] ?? 'C:/Users/You/Music';
  const tracks = [];
  albums.forEach((a, ai) => {
    for (const t of a.tracks) {
      tracks.push({
        id: tracks.length + 1, path: `${root}/${clean(a.artist)}/${clean(a.title)}/${pad(t.track)} ${clean(t.title)}.${a.ext}`,
        title: t.title, artist: a.artist, albumArtist: '', album: a.title, genre: a.genre, year: a.year, track: t.track, disc: 0,
        duration: t.duration, format: a.ext.toUpperCase(), sr: t.sr, bits: t.bits, kbps: t.kbps, lossless: t.lossless,
        size: t.size, mtime: 1714000000 + ai * 3600, ctime: 1714000000, art: true, book: false,
      });
    }
  });
  const books = BOOKS.slice(0, 2);
  books.forEach(([title, author, parts]) => {
    for (let k = 1; k <= parts; k++) {
      tracks.push({
        id: tracks.length + 1, path: `${root}/Audiobooks/${clean(title)}/${pad(k)} Chapter ${k}.mp3`, title: `Chapter ${k}`,
        artist: author, albumArtist: '', album: title, genre: 'Audiobook', year: 2015, track: k, disc: 0, duration: 1500000,
        format: 'MP3', sr: 44100, bits: 0, kbps: 64, lossless: false, size: 12000000, mtime: 1714000000, ctime: 1714000000,
        art: true, book: true,
      });
    }
  });
  return { ...group(tracks, 'local', []), roots };
}

export function playlistItems(index) {
  const music = index.tracks.filter((t) => !t.book).map((t) => t.path);
  const r = rng(99);
  const pickN = (n) => Array.from({ length: n }, () => music[Math.floor(r() * music.length)]).filter((p, i, a) => a.indexOf(p) === i);
  return { 'Road trip': pickN(24), 'Late night': pickN(16), Running: pickN(31) };
}

// ---- covers -----------------------------------------------------------------------------------

const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
const hex = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));

// A square cover: a diagonal gradient between two colours with one of five simple motifs.
export function coverPng(seedText, size = 256) {
  const h = createHash('sha1').update(seedText).digest();
  const [c1, c2] = PALETTE[h[0] % PALETTE.length].map(hex);
  const style = h[1] % 5;
  const raw = Buffer.alloc((size * 3 + 1) * size);
  const cx = size * (0.3 + (h[2] / 255) * 0.4); const cy = size * (0.3 + (h[3] / 255) * 0.4);
  for (let y = 0; y < size; y++) {
    raw[y * (size * 3 + 1)] = 0;
    for (let x = 0; x < size; x++) {
      let f = (x + y) / (2 * size);
      const dx = x - cx; const dy = y - cy; const d = Math.sqrt(dx * dx + dy * dy) / size;
      let light = 0;
      switch (style) {
        case 0: if (d < 0.22) light = 0.35; else if (d < 0.24) light = 0.6; break;
        case 1: if (Math.floor((x + y) / (size / 8)) % 2 === 0) light = 0.1; break;
        case 2: light = Math.max(0, 0.45 - d) * 0.9; break;
        case 3: if (Math.abs(Math.sin(d * 28)) < 0.18) light = 0.25; break;
        default: f = y / size; if (y > size * 0.62 && y < size * 0.66) light = 0.5; break;
      }
      const o = y * (size * 3 + 1) + 1 + x * 3;
      for (let k = 0; k < 3; k++) {
        const base = c1[k] * (1 - f) + c2[k] * f;
        raw[o + k] = Math.round(base + (255 - base) * light);
      }
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return {
    png: Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]),
    color: `#${[0, 1, 2].map((k) => Math.round((c1[k] + c2[k]) / 2).toString(16).padStart(2, '0')).join('')}`,
  };
}
