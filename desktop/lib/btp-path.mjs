// BTP/1 path rules (docs/SYNC_PROTOCOL.md section 9.2), checked on the computer before a
// command is started, so the UI can show the problem next to the file. The device checks
// again; this copy only exists to give a better message earlier.
//
// checkPath(p) returns null when the path is valid, or the reason name used in
// tests/btp/vectors.json. p is a string or a Uint8Array of UTF-8 bytes.

const MAX_PATH_BYTES = 1024;
const MAX_COMPONENTS = 32;
const MAX_COMPONENT_BYTES = 255;
const FORBIDDEN = new Set(['\\', ':', '*', '?', '"', '<', '>', '|']);

const enc = new TextEncoder();

function toString(p) {
  if (typeof p === 'string') {
    if (typeof p.isWellFormed === 'function' && !p.isWellFormed()) return null;
    return p;
  }
  try {
    return new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(p);
  } catch {
    return null;
  }
}

export function checkPath(p, { allowEmpty = false } = {}) {
  const s = toString(p);
  const bytes = typeof p === 'string' ? (s === null ? null : enc.encode(s)) : p;
  if (bytes !== null && bytes.length === 0) return allowEmpty ? null : 'empty';
  if (bytes !== null && bytes.length > MAX_PATH_BYTES) return 'too_long';
  if (s === null) return 'bad_utf8';
  if (s.startsWith('/')) return 'absolute';
  const parts = s.split('/');
  if (parts.length > MAX_COMPONENTS) return 'too_deep';
  for (const c of parts) {
    if (c === '') return 'empty_component';
    if (c === '.' || c === '..') return 'dot_component';
    if (c.startsWith('.')) return 'hidden';
    for (const ch of c) {
      const code = ch.codePointAt(0);
      if (code <= 0x1f || code === 0x7f) return 'control_char';
      if (FORBIDDEN.has(ch)) return 'forbidden_char';
    }
    if (c.endsWith(' ') || c.endsWith('.')) return 'trailing_dot_space';
    if (enc.encode(c).length > MAX_COMPONENT_BYTES) return 'component_too_long';
  }
  return null;
}

// Joins a folder path and a name into a BTP path ("" is the SD root).
export function joinPath(dir, name) {
  return dir ? `${dir}/${name}` : name;
}

// The parent of a BTP path ("" for a top-level name).
export function parentPath(p) {
  const i = p.lastIndexOf('/');
  return i < 0 ? '' : p.slice(0, i);
}

// Clients must send NFC (SYNC_PROTOCOL 9.2).
export function normalise(p) {
  return p.normalize('NFC');
}
