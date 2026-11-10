// Iconos del launcher de hibyshell (PNG con alfa, tamanos 48/56/64), mismo estilo que el prototipo.
// Uso: node launcher_icons.js <salida_dir>   (playwright-core en PWCORE, Chromium de ms-playwright)
const { chromium } = require(process.env.PWCORE || 'playwright-core');
const fs = require('fs'), path = require('path');
const out = process.argv[2];
fs.mkdirSync(out, { recursive: true });
const exe = fs.readdirSync(path.join(process.env.LOCALAPPDATA, 'ms-playwright', 'chromium-1243'))
  .map(d => path.join(process.env.LOCALAPPDATA, 'ms-playwright', 'chromium-1243', d, 'chrome.exe')).find(fs.existsSync);

const gear = (() => { const n = 8, R = 9.8, r = 7.2; let d = '';
  for (let i = 0; i < n; i++) { const a = i * 2 * Math.PI / n;
    [[r, a - .36], [R, a - .2], [R, a + .2], [r, a + .36]].forEach(([q, t], j) => { d += (i === 0 && j === 0 ? 'M' : 'L') + (12 + q * Math.cos(t)).toFixed(2) + ' ' + (12 + q * Math.sin(t)).toFixed(2); }); }
  return d + 'zM12 8.6a3.4 3.4 0 100 6.8 3.4 3.4 0 000-6.8z'; })();
// [trazo, relleno]
const G = {
  note: ['M9 17.5V6.2l11-2.2v11.3M9 17.5a2.7 2.7 0 11-5.4 0 2.7 2.7 0 015.4 0zM20 15.3a2.7 2.7 0 11-5.4 0 2.7 2.7 0 015.4 0z', ''],
  gear: [gear, ''],
  cast: ['M3 17a4 4 0 014 4M3 13a8 8 0 018 8M3 9.5V6.2C3 5 4 4 5.2 4h13.6C20 4 21 5 21 6.2v11.6c0 1.2-1 2.2-2.2 2.2H14', ''],
  book: ['M4.5 5.5A2 2 0 016.5 3.5h13v15h-13a2 2 0 00-2 2zM4.5 20.5v-15M8.5 7.5h7', ''],
  dac: ['M5 3.5h14v17H5zM9 7.5h6M12 11.5a3 3 0 100 6 3 3 0 000-6z', ''],
  waves: ['M3 9c2-2.2 4-2.2 6 0s4 2.2 6 0 4-2.2 6 0M3 15c2-2.2 4-2.2 6 0s4 2.2 6 0 4-2.2 6 0', ''],
  disc: ['M12 3.2a8.8 8.8 0 100 17.6 8.8 8.8 0 000-17.6zM12 9.6a2.4 2.4 0 100 4.8 2.4 2.4 0 000-4.8zM16.6 16.6l3 3', ''],
  play: ['M4 9v6M8 6v12M12 9v6M16 4v16M20 8v8', ''],
  sliders: ['M4 7h9M17 7h3M4 17h3M11 17h9M4 12h13M21 12h-0M15 4.5v5M9 14.5v5M19 9.5v5', ''],
  store: ['M7.5 19.5l4.5-15 4.5 15M9.3 13.5h5.4M4 19.5h16', ''],
  freemusic: ['M8 15.5V5.5l9-2v9.5M8 15.5a2.4 2.4 0 11-4.8 0 2.4 2.4 0 014.8 0zM17 13a2.4 2.4 0 11-4.8 0 2.4 2.4 0 014.8 0zM18.5 16v5M16.3 18.8l2.2 2.2 2.2-2.2', ''],
  radio: ['M5 9.5h14a1.5 1.5 0 011.5 1.5v8a1.5 1.5 0 01-1.5 1.5H5A1.5 1.5 0 013.5 19v-8A1.5 1.5 0 015 9.5zM7 9.5l9-5.5M15.5 15a2 2 0 100 .01M7 13.5h4M7 16.5h4', ''],
};
// fondo [c1, c2], glifo, color del glifo
const APPS = {
  music:      [['#ff6b81', '#fa1e45'], 'note',  '#fff'],
  settings:   [['#c7c7cc', '#6e6e73'], 'gear',  '#fff'],
  wireless:   [['#5ef08a', '#14a64a'], 'cast',  '#fff'],
  books:      [['#ffbf47', '#ff7a00'], 'book',  '#fff'],
  dac:        [['#a5a5ad', '#55555c'], 'dac',   '#fff'],
  tidal:      [['#2b2b2f', '#000000'], 'waves', '#fff'],
  qobuz:      [['#2f6bff', '#0a2f99'], 'disc',  '#fff'],
  nowplaying: [['#ff8a5c', '#e5306b'], 'play',  '#fff'],
  radio:      [['#c58cff', '#7b3fe4'], 'radio', '#fff'],
  personalize:[['#8e8e93', '#3a3a3c'], 'sliders', '#fff'],
  appstore:   [['#4fb3ff', '#1d6cf2'], 'store', '#fff'],
  freemusic:  [['#3ee0c0', '#0b8f8a'], 'freemusic', '#fff'],
};

function shell(size, c1, c2, inner) {
  const r = size * 0.2237;
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">
  <defs><linearGradient id="g" x1="0" y1="0" x2="0.35" y2="1"><stop offset="0" stop-color="${c1}"/><stop offset="1" stop-color="${c2}"/></linearGradient>
  <linearGradient id="h" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="rgba(255,255,255,.30)"/><stop offset=".5" stop-color="rgba(255,255,255,0)"/></linearGradient></defs>
  <rect width="${size}" height="${size}" rx="${r}" fill="url(#g)"/>
  ${inner}
  <rect width="${size}" height="${size}" rx="${r}" fill="url(#h)"/>
  <rect x=".5" y=".5" width="${size - 1}" height="${size - 1}" rx="${r - .5}" fill="none" stroke="rgba(255,255,255,.28)"/></svg>`;
}
function glyphIcon(size, [c1, c2], glyph, col) {
  const g = size * 0.5, o = (size - g) / 2, sw = 2.0 * 24 / g;
  return shell(size, c1, c2, `<g transform="translate(${o} ${o}) scale(${g / 24})"><path d="${G[glyph][0]}" fill="none" stroke="${col}" stroke-width="${sw}" stroke-linecap="round" stroke-linejoin="round"/></g>`);
}
// Archivos: fondo claro con carpeta azul (pestana + cuerpo con degradado)
function filesIcon(size) {
  const s = size / 56;
  const inner = `<defs><linearGradient id="f1" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5ac8fa"/><stop offset="1" stop-color="#1a7cf5"/></linearGradient>
    <linearGradient id="f2" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7fd6ff"/><stop offset="1" stop-color="#2f8ef7"/></linearGradient></defs>
    <g transform="scale(${s})">
      <path d="M10 17.5a3 3 0 013-3h9.2a3 3 0 012.3 1.1l2 2.4H43a3 3 0 013 3V24H10z" fill="url(#f1)"/>
      <rect x="10" y="21.5" width="36" height="21" rx="3.2" fill="url(#f2)"/>
      <rect x="10" y="21.5" width="36" height="1.2" fill="rgba(255,255,255,.55)"/>
    </g>`;
  return shell(size, '#ffffff', '#d9dce3', inner);
}

(async () => {
  const b = await chromium.launch({ executablePath: exe });
  const p = await b.newPage({ deviceScaleFactor: 1 });
  for (const size of [48, 56, 64]) {
    const all = Object.entries(APPS).map(([n, a]) => [n, glyphIcon(size, ...a)]);
    all.push(['files', filesIcon(size)]);
    for (const [name, svg] of all) {
      await p.setContent(`<html><body style="margin:0;background:transparent">${svg}</body></html>`);
      const file = path.join(out, `${name}_${size}.png`);
      await (await p.$('svg')).screenshot({ path: file, omitBackground: true });
    }
    console.log('tamano', size, 'ok');
  }
  await b.close();
})();
