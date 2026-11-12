// JSON-lines reading for byos-cli output (docs/BYOS_CLI.md section 2).

export const EVENTS = new Set([
  'start', 'device', 'entry', 'progress', 'prompt', 'warn', 'result', 'error',
]);
export const TERMINAL = new Set(['result', 'error']);

// Splits a byte stream into complete UTF-8 lines. Partial lines are kept until the next
// chunk. A line ends with "\n"; a "\r" before it is dropped.
export class LineSplitter {
  constructor(onLine) {
    this.onLine = onLine;
    this.decoder = new TextDecoder('utf-8');
    this.buf = '';
  }

  push(chunk) {
    this.buf += this.decoder.decode(chunk, { stream: true });
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      let line = this.buf.slice(0, i);
      this.buf = this.buf.slice(i + 1);
      if (line.endsWith('\r')) line = line.slice(0, -1);
      if (line.length) this.onLine(line);
    }
  }

  // Call at end of stream. A last line without "\n" is still delivered.
  end() {
    this.buf += this.decoder.decode();
    const line = this.buf.replace(/\r$/, '');
    this.buf = '';
    if (line.length) this.onLine(line);
  }
}

// Parses and checks one line. Returns { ok: true, event } or { ok: false, reason }.
export function parseEventLine(line) {
  let obj;
  try {
    obj = JSON.parse(line);
  } catch {
    return { ok: false, reason: 'not JSON' };
  }
  if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) {
    return { ok: false, reason: 'not an object' };
  }
  if (obj.v !== 1) return { ok: false, reason: `unknown output version ${JSON.stringify(obj.v)}` };
  if (!EVENTS.has(obj.event)) return { ok: false, reason: `unknown event ${JSON.stringify(obj.event)}` };
  if (obj.event === 'result' && obj.ok !== true) return { ok: false, reason: 'result without ok:true' };
  if (obj.event === 'error' && (obj.ok !== false || typeof obj.code !== 'string')) {
    return { ok: false, reason: 'error without ok:false and code' };
  }
  return { ok: true, event: obj };
}
