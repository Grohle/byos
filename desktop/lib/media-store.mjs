// The runner's own files for the Library and Music tabs: the music folders the user chose
// (settings.json), the files dropped on the window (an "incoming" folder per drop), and the
// byos-cli cache files the page may read (index, catalogue, plan, album art). Every name that
// comes from the page is checked here.

import { mkdirSync, readFileSync, renameSync, statSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { checkPath } from './btp-path.mjs';

export const DEVICE_ID = /^[0-9a-f]{16}$/;
export const ART_KEY = /^[0-9a-f]{40}-(256|500)$/;
export const SESSION = /^[A-Za-z0-9]{8,32}$/;

export function defaultSettingsDir(env = process.env, platform = process.platform, home = os.homedir()) {
  if (platform === 'win32') return path.join(env.APPDATA || path.join(home, 'AppData', 'Roaming'), 'ByOS', 'desktop');
  if (platform === 'darwin') return path.join(home, 'Library', 'Application Support', 'ByOS', 'desktop');
  return path.join(env.XDG_CONFIG_HOME || path.join(home, '.config'), 'byos-desktop');
}

export function defaultIncomingDir(env = process.env, platform = process.platform, home = os.homedir()) {
  if (platform === 'win32') return path.join(env.LOCALAPPDATA || path.join(home, 'AppData', 'Local'), 'ByOS', 'incoming');
  return path.join(os.tmpdir(), 'byos-incoming');
}

// settings.json: { "sources": [{ "path": "C:\\Users\\me\\Music", "temp": false }] }
export class Settings {
  constructor(dir) {
    this.dir = dir;
    this.file = path.join(dir, 'settings.json');
  }

  read() {
    try {
      const v = JSON.parse(readFileSync(this.file, 'utf8'));
      if (!v || typeof v !== 'object' || Array.isArray(v)) return { sources: [] };
      const sources = Array.isArray(v.sources)
        ? v.sources.filter((s) => s && typeof s.path === 'string' && path.isAbsolute(s.path)).map((s) => ({ path: s.path, temp: !!s.temp }))
        : [];
      return { ...v, sources };
    } catch {
      return { sources: [] };
    }
  }

  write(v) {
    mkdirSync(this.dir, { recursive: true });
    const tmp = `${this.file}.tmp`;
    writeFileSync(tmp, `${JSON.stringify(v, null, 2)}\n`);
    renameSync(tmp, this.file);
  }

  // The music folders that still exist (a removed drive is skipped, not forgotten).
  sources() {
    return this.read().sources.map((s) => ({ ...s, exists: isDir(s.path) }));
  }

  addSource(p, temp = false) {
    if (typeof p !== 'string' || !path.isAbsolute(p) || !isDir(p)) throw new Error('not a folder');
    const v = this.read();
    const norm = path.resolve(p);
    if (!v.sources.some((s) => path.resolve(s.path).toLowerCase() === norm.toLowerCase())) v.sources.push({ path: norm, temp });
    this.write(v);
    return this.sources();
  }

  removeSource(p) {
    const v = this.read();
    v.sources = v.sources.filter((s) => path.resolve(s.path).toLowerCase() !== path.resolve(String(p)).toLowerCase());
    this.write(v);
    return this.sources();
  }
}

export function isDir(p) {
  try { return statSync(p).isDirectory(); } catch { return false; }
}

// A dropped file's place under the session's incoming folder: "Album/01 Song.flac" checked
// component by component with the card's own name rules (no "..", no hidden names).
export function incomingTarget(base, session, rel) {
  if (typeof session !== 'string' || !SESSION.test(session)) return null;
  if (typeof rel !== 'string' || !rel || rel.length > 1024) return null;
  const parts = rel.normalize('NFC').split('/');
  if (parts.length > 16 || parts.some((c) => !c || checkPath(c))) return null;
  const root = path.join(base, session);
  const full = path.join(root, ...parts);
  if (!full.startsWith(root + path.sep)) return null;
  return { root, full };
}

// The cache file for a page request, or null when the name is not allowed.
export function cacheFile(cache, kind, name) {
  switch (kind) {
    case 'library': return DEVICE_ID.test(name ?? '') ? path.join(cache, 'library', name, 'index.json') : null;
    case 'art': return ART_KEY.test(name ?? '') ? path.join(cache, 'art', `${name}.jpg`) : null;
    case 'catalog': return path.join(cache, 'music', 'catalog.json');
    // the Sync tab's linked-folder plan (sync-plan --out)
    case 'plan': return path.join(cache, 'sync', 'plan-v2.json');
    default: return null;
  }
}

// What the bytes of an image are (the demo draws PNG covers; byos-cli writes JPEG).
export function imageType(buf) {
  if (buf.length >= 8 && buf[0] === 0x89 && buf[1] === 0x50 && buf[2] === 0x4e && buf[3] === 0x47) return 'image/png';
  if (buf.length >= 3 && buf[0] === 0xff && buf[1] === 0xd8) return 'image/jpeg';
  return null;
}
