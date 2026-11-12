// Demo mode's GitHub for the app's own update (server.mjs --mock, and the tests): a release list,
// a signed index and a small "ByOS.exe", served from memory, signed with a key made at start.
// Nothing here touches the network.

import { createHash, generateKeyPairSync, sign } from 'node:crypto';

export const DEMO_API = 'https://api.github.invalid/repos/demo/releases?per_page=100';
export const DEMO_DOWNLOADS = 'https://github.invalid/demo/releases/download/';

// Fills in the "ed25519:" + 128 zeros placeholder of a JSON text (docs/OTA.md).
export function signJson(text, privateKey) {
  const zeros = '0'.repeat(128);
  const at = text.indexOf(`ed25519:${zeros}`);
  if (at < 0) throw new Error('no signature placeholder');
  const sig = sign(null, Buffer.from(text, 'utf8'), privateKey).toString('hex');
  return Buffer.from(text.replace(`ed25519:${zeros}`, `ed25519:${sig}`), 'utf8');
}

export function newKey() {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519');
  const pub = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url').toString('hex');
  return { pub, privateKey };
}

export function indexText({ version, tag = `desktop-v${version}`, asset = 'ByOS.exe', size, sha256, app = 'byos-desktop-windows',
  product = 'byos-app-update', format = 1, minOs = '10.0.17134', notes } = {}) {
  return `${JSON.stringify({
    format, product, app, version, tag, asset, size, sha256, min_os: minOs, published: '2026-11-12',
    notes: notes ?? {
      en: 'Faster sync and a Check for Updates button.',
      es: 'Sincronización más rápida y un botón Buscar actualizaciones.',
      fr: 'Synchro plus rapide et un bouton Rechercher des mises à jour.',
    },
    signature: `ed25519:${'0'.repeat(128)}`,
  }, null, 1)}\n`;
}

// A fetch() that answers like GitHub. files: url -> Buffer; api: the release list (array).
export function fakeGitHub({ files = new Map(), releases = [], apiStatus = 200, apiHeaders = {}, delayMs = 0, chunk = 65536 } = {}) {
  const calls = [];
  const fake = async (url, opts = {}) => {
    calls.push(String(url));
    if (delayMs) await new Promise((r) => setTimeout(r, delayMs));
    if (opts.signal?.aborted) throw Object.assign(new Error('aborted'), { name: 'AbortError' });
    const api = String(url) === fake.api;
    if (api && fake.apiStatus !== 200) return new Response(null, { status: fake.apiStatus, headers: fake.apiHeaders });
    const body = api ? Buffer.from(JSON.stringify(fake.releases)) : fake.files.get(String(url));
    if (!body) return new Response('Not Found', { status: 404 });
    let sent = 0;
    const stream = new ReadableStream({
      async pull(c) {
        if (opts.signal?.aborted) { c.error(Object.assign(new Error('aborted'), { name: 'AbortError' })); return; }
        if (sent >= body.length) { c.close(); return; }
        if (delayMs) await new Promise((r) => setTimeout(r, delayMs));
        c.enqueue(new Uint8Array(body.subarray(sent, sent + chunk)));
        sent += chunk;
      },
    });
    return new Response(stream, { status: 200, headers: { 'content-length': String(body.length), 'content-type': api ? 'application/json' : 'application/octet-stream' } });
  };
  Object.assign(fake, { files, releases, apiStatus, apiHeaders, calls, api: DEMO_API });
  return fake;
}

// The whole demo: ByOS Desktop `latest` signed and served (the demo app says it is `current`).
export function demoRelease({ current = '0.9.0', latest = '0.9.1', size = 3 * 1024 * 1024, delayMs = 15 } = {}) {
  const key = newKey();
  const exe = Buffer.alloc(size, 7);
  exe.write('MZ demo ByOS.exe', 0);
  const sha256 = createHash('sha256').update(exe).digest('hex');
  const tag = `desktop-v${latest}`;
  const files = new Map([
    [`${DEMO_DOWNLOADS}${tag}/byos-app-update.json`, signJson(indexText({ version: latest, size: exe.length, sha256 }), key.privateKey)],
    [`${DEMO_DOWNLOADS}${tag}/ByOS.exe`, exe],
  ]);
  const releases = [
    { tag_name: 'v0.9.1', draft: false, prerelease: false, assets: [{ name: 'hibyos-update.json' }] },
    { tag_name: tag, draft: false, prerelease: false, assets: [{ name: 'ByOS.exe' }, { name: 'byos-app-update.json' }] },
    { tag_name: `desktop-v${current}`, draft: false, prerelease: false, assets: [{ name: 'ByOS.exe' }, { name: 'byos-app-update.json' }] },
  ];
  return { key, exe, sha256, fetch: fakeGitHub({ files, releases, delayMs, chunk: 128 * 1024 }), current, latest };
}
