// The Library and Music tabs: argument checks, the runner's media endpoints (against the fake
// byos-cli), the shared formatting rules and the translations.

import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { buildArgs, UsageError } from '../lib/cli.mjs';
import {
  age, fmtDuration, fold, groupIndex, libraryState, matches, planSummary, quality, scanEstimate, selectionJson,
  syncOutcome, textOn, virtualWindow,
} from '../lib/media-format.mjs';
import { cacheFile, imageType, incomingTarget, Settings } from '../lib/media-store.mjs';
import { createApp } from '../server.mjs';
import { STRINGS, setLang, t } from '../ui/i18n.js';

const tmp = mkdtempSync(path.join(os.tmpdir(), 'byos-media-test-'));
const music = path.join(tmp, 'My Music');
mkdirSync(music, { recursive: true });

// ---- buildArgs --------------------------------------------------------------------------------

test('library commands: remote paths are checked, page files are refused', () => {
  assert.deepEqual(buildArgs('lib-index', {}), ['lib-index']);
  assert.deepEqual(buildArgs('lib-scan', { full: true, roots: ['Music'] }), ['lib-scan', '--full', '--', 'Music']);
  assert.deepEqual(buildArgs('lib-art', { paths: ['Music/a b/1.flac'] }), ['lib-art', '--size', '256', '--', 'Music/a b/1.flac']);
  assert.throws(() => buildArgs('lib-art', { paths: ['../x'] }), UsageError);
  assert.throws(() => buildArgs('lib-art', { paths: Array(65).fill('a.flac') }), UsageError);
  assert.throws(() => buildArgs('lib-art', { paths: ['a.flac'], size: 1024 }), UsageError);
  assert.deepEqual(buildArgs('lib-rescan', { full: true }), ['lib-rescan', '--full']);
  assert.deepEqual(buildArgs('lib-rescan', { paths: ['-r'] }), ['lib-rescan', '--', '-r']);
  assert.throws(() => buildArgs('lib-rescan', {}), UsageError);
  assert.deepEqual(buildArgs('rm', { paths: ['Music/x.flac'], yes: true, pruneEmpty: true }),
    ['rm', '--yes', '--prune-empty', '--', 'Music/x.flac']);
  assert.deepEqual(buildArgs('playlist-add', { name: 'Road trip', paths: ['Music/a.flac'], create: true }),
    ['playlist-add', '--create', '--', 'Road trip', 'Music/a.flac']);
  assert.deepEqual(buildArgs('playlist-rm', { name: 'Road trip' }), ['playlist-rm', '--', 'Road trip']);
  for (const bad of ['', 'a/b', '..', 'x?', '.hidden', 42]) assert.throws(() => buildArgs('playlist-add', { name: bad, paths: ['a.flac'] }), UsageError);
  assert.deepEqual(buildArgs('art-fix', { dryRun: true }), ['art-fix', '--root', 'Music', '--dry-run']);
  assert.deepEqual(buildArgs('cover-cache-reset', {}), ['cover-cache-reset']);
});

test('music commands: only existing absolute folders and runner files', () => {
  assert.deepEqual(buildArgs('music-scan', { sources: [music] }), ['music-scan', '--', music]);
  assert.throws(() => buildArgs('music-scan', { sources: ['relative/dir'] }), UsageError);
  assert.throws(() => buildArgs('music-scan', { sources: [path.join(tmp, 'missing')] }), UsageError);
  const file = path.join(tmp, 'f.txt');
  writeFileSync(file, 'x');
  assert.throws(() => buildArgs('music-scan', { sources: [file] }), UsageError);
  assert.throws(() => buildArgs('music-scan', {}), UsageError);
  const cat = path.join(tmp, 'catalog.json');
  assert.deepEqual(buildArgs('sync-plan', { catalog: cat, dest: 'Music', removeUnselected: true }),
    ['sync-plan', '--catalog', cat, '--dest', 'Music', '--layout', 'artist-album', '--covers', 'optimize', '--remove-unselected']);
  assert.throws(() => buildArgs('sync-plan', { catalog: 'catalog.json' }), UsageError);
  assert.throws(() => buildArgs('sync-plan', { catalog: cat, dest: '../out' }), UsageError);
  assert.throws(() => buildArgs('sync-plan', { catalog: cat, layout: 'flat' }), UsageError);
  assert.deepEqual(buildArgs('sync-apply', { plan: cat, lyrics: true, then: 'rescan', yes: true }),
    ['sync-apply', '--covers', 'optimize', '--then', 'rescan', '--lyrics', '--yes', '--', cat]);
  assert.throws(() => buildArgs('sync-apply', { plan: cat, then: 'reboot' }), UsageError);
});

// ---- the runner's files ---------------------------------------------------------------------------

test('cache names and dropped files cannot leave their folders', () => {
  const cache = path.join(tmp, 'cache');
  assert.equal(cacheFile(cache, 'library', '0123456789abcdef'), path.join(cache, 'library', '0123456789abcdef', 'index.json'));
  for (const bad of ['..', '0123456789ABCDEF', '0123456789abcde', '../../x', '0123456789abcdef/..']) assert.equal(cacheFile(cache, 'library', bad), null);
  assert.equal(cacheFile(cache, 'art', `${'a'.repeat(40)}-256`), path.join(cache, 'art', `${'a'.repeat(40)}-256.jpg`));
  for (const bad of [`${'a'.repeat(40)}-100`, '../x-256', `${'g'.repeat(40)}-256`]) assert.equal(cacheFile(cache, 'art', bad), null);
  assert.equal(cacheFile(cache, 'secrets'), null);
  const inc = path.join(tmp, 'incoming');
  assert.equal(incomingTarget(inc, 'abcd1234', 'Album/01 Song.flac').full, path.join(inc, 'abcd1234', 'Album', '01 Song.flac'));
  for (const rel of ['../x.flac', 'a/../../x', '.hidden/x.flac', 'a//b', 'C:/x', '/abs', 'a\\..\\b']) assert.equal(incomingTarget(inc, 'abcd1234', rel), null, rel);
  assert.equal(incomingTarget(inc, '../x', 'a.flac'), null);
  assert.equal(imageType(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0, 0, 0, 0])), 'image/png');
  assert.equal(imageType(Buffer.from([0xff, 0xd8, 0xff])), 'image/jpeg');
  assert.equal(imageType(Buffer.from('<html>')), null);
});

test('settings keep the music folders', () => {
  const s = new Settings(path.join(tmp, 'settings'));
  assert.deepEqual(s.sources(), []);
  s.addSource(music);
  s.addSource(music); // no duplicate
  assert.equal(s.sources().length, 1);
  assert.equal(s.sources()[0].exists, true);
  assert.throws(() => s.addSource('relative'));
  assert.throws(() => s.addSource(path.join(tmp, 'nope')));
  writeFileSync(s.file, '{"sources":[{"path":"rel"},{"path":"' + JSON.stringify(path.join(tmp, 'gone')).slice(1, -1) + '"}]}');
  assert.deepEqual(s.sources().map((x) => x.exists), [false]);
  s.removeSource(path.join(tmp, 'gone'));
  assert.deepEqual(s.sources(), []);
  writeFileSync(s.file, 'not json');
  assert.deepEqual(s.sources(), []);
});

// ---- the runner, end to end with the fake byos-cli -------------------------------------------------

const env = { ...process.env, BYOS_MOCK_STATE: path.join(tmp, 'state.json') };
const app = createApp({
  mock: true, env, token: 'testtoken', config: path.join(tmp, 'cli'), settingsDir: path.join(tmp, 'desk'),
  incomingDir: path.join(tmp, 'incoming'), pickFolder: async () => music,
});
let base;
test.before(async () => { base = await app.listen(0); });
test.after(async () => { await app.close(); rmSync(tmp, { recursive: true, force: true }); });
const H = { 'X-ByOS-Token': 'testtoken', 'Content-Type': 'application/json' };
const call = async (method, p, body) => {
  const r = await fetch(new URL(p, base), { method, headers: H, body: body === undefined ? undefined : JSON.stringify(body) });
  const text = await r.text();
  let json = null;
  try { json = JSON.parse(text); } catch { /* binary */ }
  return { status: r.status, json, type: r.headers.get('content-type') };
};
async function job(command, params) {
  const r = await call('POST', '/api/jobs', { command, params });
  assert.equal(r.status, 202, JSON.stringify(r.json));
  const ev = await fetch(new URL(`/api/jobs/${r.json.id}/events?token=testtoken`, base));
  const text = await ev.text();
  const end = text.split('\n\n').find((b) => b.startsWith('event: end'));
  return JSON.parse(/^data: (.*)$/m.exec(end)[1]);
}

test('library: index, cached art, and refused names', async () => {
  assert.equal((await call('GET', '/api/library/0123456789abcdef')).status, 404);
  const final = await job('lib-index', { device: 'usb' });
  assert.equal(final.ok, true);
  const idx = await call('GET', '/api/library/0123456789abcdef');
  assert.equal(idx.status, 200);
  assert.ok(idx.json.albums.length >= 30);
  assert.equal((await call('GET', '/api/library/..%2F..%2Fsecrets')).status, 400);
  assert.equal((await call('GET', '/api/library/0123456789ABCDEF')).status, 400);
  const art = await job('lib-art', { device: 'usb', paths: [idx.json.albums[0].art] });
  assert.equal(art.ok, true);
  const img = await call('GET', `/api/art/${art.items[0].key}`);
  assert.equal(img.status, 200);
  assert.equal(img.type, 'image/png');
  assert.equal((await call('GET', '/api/art/..%2Fstate')).status, 400);
  assert.equal((await call('GET', `/api/art/${'0'.repeat(40)}-256`)).status, 404);
  // the page cannot point byos-cli at files on this computer
  const r = await job('lib-rescan', { device: 'usb', full: true });
  assert.equal(r.btp.name, 'E_UNSUPPORTED');
});

test('music: folders from the settings, catalogue, plan and sync', async () => {
  assert.equal((await call('POST', '/api/jobs', { command: 'music-scan', params: {} })).status, 400); // no folder yet
  const picked = await call('POST', '/api/pick-folder', {});
  assert.equal(picked.json.picked, music);
  assert.equal(picked.json.sources.length, 1);
  // sources sent by the page are ignored: the runner's own list is used
  const scan = await job('music-scan', { sources: ['C:\\Windows'] });
  assert.equal(scan.ok, true);
  assert.deepEqual(scan.roots, [music]);
  const cat = await call('GET', '/api/catalog');
  assert.ok(cat.json.tracks.length > 100);
  const plan = await job('sync-plan', { device: 'usb', selection: { all: false, artists: ['Aurora Fields'], books: false }, catalog: 'C:\\evil.json' });
  assert.equal(plan.ok, true);
  assert.equal(plan.plan, path.join(app.cache, 'sync', 'plan.json'));
  assert.ok(plan.add > 0 && plan.add < 60, String(plan.add));
  assert.deepEqual(JSON.parse(readFileSync(path.join(app.cache, 'sync', 'selection.json'), 'utf8')).artists, ['Aurora Fields']);
  assert.equal((await call('POST', '/api/jobs', { command: 'sync-plan', params: { selection: [1] } })).status, 400);
  const apply = await job('sync-apply', { device: 'usb', then: 'rescan', yes: true, plan: 'C:\\evil.json' });
  assert.equal(apply.ok, true);
  assert.equal(syncOutcome(apply), 'library-pending');
  assert.equal(apply.written, plan.add + plan.update);
  // dropped files: one folder per drop, then a temporary source
  const drop = await fetch(new URL('/api/incoming?session=abcd1234&rel=Album%2F01%20Song.flac', base), { method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: 'xx' });
  assert.equal(drop.status, 200);
  const bad = await fetch(new URL('/api/incoming?session=abcd1234&rel=..%2Fx.flac', base), { method: 'POST', headers: { 'X-ByOS-Token': 'testtoken' }, body: 'xx' });
  assert.equal(bad.status, 400);
  const done = await call('POST', '/api/incoming/done?session=abcd1234', {});
  const temp = done.json.sources.find((s) => s.temp);
  assert.ok(existsSync(path.join(temp.path, 'Album', '01 Song.flac')));
  const after = await call('DELETE', '/api/sources', { path: temp.path });
  assert.equal(after.json.sources.length, 1);
  assert.equal(existsSync(temp.path), false);
});

// ---- formatting rules ----------------------------------------------------------------------------------

test('plan summary in English, Spanish and French', () => {
  const plan = { add: 312, update: 4, remove: 0, bytes_add: 2.1e9, sd_free: 58e9, fits: true };
  setLang('en');
  assert.equal(planSummary(plan, t, 'en'), 'Add 312 songs (2.1 GB) · update 4 · remove 0 · 58 GB free');
  setLang('es');
  assert.equal(planSummary(plan, t, 'es'), 'Añadir 312 canciones (2,1 GB) · actualizar 4 · eliminar 0 · 58 GB libres');
  setLang('fr');
  assert.equal(planSummary({ ...plan, add: 1312 }, t, 'fr'), 'Ajouter 1\u202f312 morceaux (2,1 GB) · mettre à jour 4 · supprimer 0 · 58 GB libres');
  setLang('en');
});

test('every new string exists in English, Spanish and French', () => {
  const keys = Object.keys(STRINGS.en).filter((k) => /^(lib|view|col|del|add|new|remove|delete|playlist|get|info|rescan|cover|fix|reset|music|sync|opt|layout|plan|apply|retry|library|cap|left|rate|howto|age|total|unknown|chapters|songs|albums|sel|not)/.test(k));
  assert.ok(keys.length > 100);
  for (const l of ['fr', 'es']) {
    const missing = keys.filter((k) => !(k in STRINGS[l]));
    assert.deepEqual(missing, [], l);
    // the same {placeholders}
    for (const k of keys) {
      const ph = (s) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');
      assert.equal(ph(STRINGS[l][k]), ph(STRINGS.en[k]), `${l}.${k}`);
    }
  }
});

test('formatting helpers', () => {
  assert.equal(fmtDuration(225000), '3:45');
  assert.equal(fmtDuration(3723000), '1:02:03');
  assert.equal(fmtDuration(0), '');
  assert.ok(matches('beyonce halo', 'Halo', 'Beyoncé'));
  assert.ok(!matches('beyonce x', 'Halo', 'Beyoncé'));
  assert.equal(fold('Ñandú'), 'nandu');
  assert.equal(quality({ format: 'FLAC', lossless: true, sr: 96000, bits: 24 }), 'FLAC 24/96');
  assert.equal(quality({ format: 'FLAC', lossless: true, sr: 44100, bits: 16 }), 'FLAC 16/44.1');
  assert.equal(quality({ format: 'MP3', lossless: false, kbps: 320 }), 'MP3 320 kbps');
  assert.deepEqual(virtualWindow(0, 280, 28, 1000, 2), { start: 0, end: 12, top: 0, height: 28000 });
  assert.deepEqual(virtualWindow(2800, 280, 28, 1000, 2), { start: 98, end: 112, top: 2744, height: 28000 });
  assert.deepEqual(virtualWindow(27900, 280, 28, 1000, 2), { start: 994, end: 1000, top: 27832, height: 28000 });
  assert.equal(age(1000 - 30, 1000, t), 'just now');
  assert.equal(age(1000 - 125, 1000, t), '2 min ago');
  assert.equal(textOn('#ffffff'), 'dark');
  assert.equal(textOn('#1d1d1f'), 'light');
  assert.equal(scanEstimate(0).minutes, 1);
  assert.equal(syncOutcome({ ok: false, code: 'cancelled' }), 'cancelled');
  assert.equal(syncOutcome({ ok: true, failed: 2, library: { action: 'rescan', ok: true } }), 'partial');
  assert.equal(syncOutcome({ ok: true, failed: 0, library: { action: 'none', ok: false } }), 'done');
  assert.equal(syncOutcome({ ok: true, failed: 0, library: { action: 'rescan', ok: true } }), 'done');
  assert.equal(libraryState({ ok: false, hint: 'scan' }), 'source-unavailable');
  assert.equal(libraryState({ ok: false, code: 'timeout' }), 'error');
  assert.equal(libraryState({ ok: true }, { tracks: [] }), 'empty');
  assert.deepEqual(selectionJson({ all: false, artists: new Set(['abba']), albums: new Set(['abba\u0000arrival']), genres: new Set(), books: true }),
    { all: false, artists: ['abba'], albums: [{ artist: 'abba', title: 'arrival' }], genres: [], files: [], books: true });
});

test('grouping matches byos-cli: The ignored, album artist first, books by folder', () => {
  const tr = (path, title, artist, album, extra = {}) => ({ path, title, artist, album, albumArtist: '', genre: 'Pop', year: 2000, track: 1, disc: 0, duration: 1000, size: 1, art: true, book: false, ...extra });
  const idx = groupIndex([
    tr('Music/a/2.flac', 'B song', 'The Beatles', 'Abbey Road', { track: 2 }),
    tr('Music/a/1.flac', 'A song', 'The Beatles', 'Abbey Road', { track: 1 }),
    tr('Music/v/1.flac', 'x', 'Someone', 'Hits', { albumArtist: 'Various Artists' }),
    tr('Music/v/2.flac', 'y', 'Other', 'Hits', { albumArtist: 'Various Artists', genre: 'pop' }),
    tr('Music/z/1.flac', 'z', 'ABBA', 'Arrival'),
    tr('Audiobooks/Dune/1.mp3', 'Part 1', 'Herbert', 'Dune', { book: true }),
    tr('Audiobooks/Dune/2.mp3', 'Part 2', 'Herbert', 'Dune', { book: true, track: 2 }),
  ]);
  assert.deepEqual(idx.albums.map((a) => a.title), ['Abbey Road', 'Arrival', 'Hits']);
  assert.deepEqual(idx.albums[0].tracks.map((i) => idx.tracks[i].title), ['A song', 'B song']);
  assert.deepEqual(idx.artists.map((a) => a.name), ['ABBA', 'The Beatles', 'Various Artists']);
  assert.deepEqual(idx.genres, [{ name: 'Pop', tracks: 5 }]);
  assert.equal(idx.books.length, 1);
  assert.equal(idx.books[0].tracks.length, 2);
  assert.equal(idx.counts.tracks, 5);
});
