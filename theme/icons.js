// Genera iconos PNG con transparencia a tamano exacto, dibujados por Chromium desde SVG.
// Uso: node icons.js <salida_dir>   (necesita playwright-core y el Chromium de ms-playwright)
const { chromium } = require(process.env.PWCORE || 'playwright-core');
const fs = require('fs'), path = require('path');

const out = process.argv[2];
fs.mkdirSync(out, { recursive: true });
const exe = fs.readdirSync(path.join(process.env.LOCALAPPDATA, 'ms-playwright', 'chromium-1243'))
  .map(d => path.join(process.env.LOCALAPPDATA, 'ms-playwright', 'chromium-1243', d, 'chrome.exe')).find(fs.existsSync);

// glifos (rejilla 24x24, trazo) — los mismos del prototipo
const gear = (() => { const n = 8, R = 9.8, r = 7.2; let d = '';
  for (let i = 0; i < n; i++) { const a = i * 2 * Math.PI / n;
    [[r, a - .36], [R, a - .2], [R, a + .2], [r, a + .36]].forEach(([q, t], j) => { d += (i === 0 && j === 0 ? 'M' : 'L') + (12 + q * Math.cos(t)).toFixed(2) + ' ' + (12 + q * Math.sin(t)).toFixed(2); }); }
  return d + 'zM12 8.6a3.4 3.4 0 100 6.8 3.4 3.4 0 000-6.8z'; })();
const G = {
  note: 'M9 17.5V6.2l11-2.2v11.3M9 17.5a2.7 2.7 0 11-5.4 0 2.7 2.7 0 015.4 0zM20 15.3a2.7 2.7 0 11-5.4 0 2.7 2.7 0 015.4 0z',
  gear,
  cast: 'M3 17a4 4 0 014 4M3 13a8 8 0 018 8M3 9.5V6.2C3 5 4 4 5.2 4h13.6C20 4 21 5 21 6.2v11.6c0 1.2-1 2.2-2.2 2.2H14',
  book: 'M4.5 5.5A2 2 0 016.5 3.5h13v15h-13a2 2 0 00-2 2zM4.5 20.5v-15M8.5 7.5h7',
  dac: 'M5 3.5h14v17H5zM9 7.5h6M12 11.5a3 3 0 100 6 3 3 0 000-6z',
  globe: 'M12 3.5a8.5 8.5 0 100 17 8.5 8.5 0 000-17zM3.5 12h17M12 3.5c3 3.3 3 13.7 0 17M12 3.5c-3 3.3-3 13.7 0 17',
};
const APPS = {
  music:        ['note',  '#ff6b81', '#fa1e45'],
  sys_set:      ['gear',  '#c7c7cc', '#6e6e73'],
  wireless:     ['cast',  '#5ef08a', '#14a64a'],
  book:         ['book',  '#ffbf47', '#ff7a00'],
  dac:          ['dac',   '#a5a5ad', '#55555c'],
  stream_media: ['globe', '#5ac8fa', '#0a60ff'],
};

function iconSvg(size, glyph, c1, c2, pressed) {
  const r = size * 0.2237;               // radio de esquina de iOS
  const g = size * 0.5, o = (size - g) / 2, sw = 2.0 * 24 / g;   // trazo ~2px reales
  const dark = pressed ? '<rect width="100%" height="100%" rx="' + r + '" fill="rgba(0,0,0,.28)"/>' : '';
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">
  <defs><linearGradient id="g" x1="0" y1="0" x2="0.35" y2="1"><stop offset="0" stop-color="${c1}"/><stop offset="1" stop-color="${c2}"/></linearGradient>
  <linearGradient id="h" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="rgba(255,255,255,.30)"/><stop offset=".5" stop-color="rgba(255,255,255,0)"/></linearGradient></defs>
  <rect width="${size}" height="${size}" rx="${r}" fill="url(#g)"/>
  <rect width="${size}" height="${size}" rx="${r}" fill="url(#h)"/>
  <rect x=".5" y=".5" width="${size - 1}" height="${size - 1}" rx="${r - .5}" fill="none" stroke="rgba(255,255,255,.28)"/>
  <g transform="translate(${o} ${o}) scale(${g / 24})"><path d="${G[glyph]}" fill="none" stroke="#fff" stroke-width="${sw}" stroke-linecap="round" stroke-linejoin="round"/></g>
  ${dark}</svg>`;
}

(async () => {
  const b = await chromium.launch({ executablePath: exe });
  const p = await b.newPage({ deviceScaleFactor: 1 });
  const size = parseInt(process.env.ICON_SIZE || '56');
  for (const [name, [glyph, c1, c2]] of Object.entries(APPS)) {
    for (const pressed of [false, true]) {
      await p.setContent(`<html><body style="margin:0;background:transparent">${iconSvg(size, glyph, c1, c2, pressed)}</body></html>`);
      const el = await p.$('svg');
      const file = path.join(out, name + (pressed ? '_s' : '') + '.png');
      await el.screenshot({ path: file, omitBackground: true });
      console.log(file);
    }
  }
  await b.close();
})();
