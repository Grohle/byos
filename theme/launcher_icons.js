// Iconos del launcher de ByOS (PNG con alfa, 48/56/64 px) + glifos del Centro de control (26 px).
// Uso: node launcher_icons.js <salida_dir> [tamanos]   (playwright-core en PWCORE, Chromium de ms-playwright)
//   salida_dir/<app>_<tam>.png   iconos del launcher (mascara squircle ya aplicada)
//   salida_dir/cc/cc_<n>.png     glifos del Centro de control (blancos, 26x26)
//   tamanos: lista separada por comas (por defecto 48,56,64); p. ej. "192" para la hoja de contacto a 3x
//
// Sistema comun (rejilla de 100 unidades, como la plantilla de 1024 de iOS):
//  - mascara: superelipse n=5 ("squircle", esquinas continuas), no un rectangulo redondeado
//  - keylines: circulo r=32 (64 %), cuadrado 58, vertical 50x66, horizontal 66x50
//  - luz desde arriba: fondos mas claros arriba, sombras suaves hacia abajo (dy>0), brillos en bordes superiores
//  - sin brillo "gloss" de iOS 6: solo un filo de luz de 1/2 px en el borde superior interior
const { chromium } = require(process.env.PWCORE || 'playwright-core');
const fs = require('fs'), path = require('path');
const out = process.argv[2];
const SIZES = (process.argv[3] || '48,56,64').split(',').map(Number);
fs.mkdirSync(out, { recursive: true });
const msp = path.join(process.env.LOCALAPPDATA, 'ms-playwright');
const exe = fs.readdirSync(msp).filter(d => d.startsWith('chromium-')).sort().reverse()
  .flatMap(d => fs.readdirSync(path.join(msp, d)).map(e => path.join(msp, d, e, 'chrome.exe'))).find(fs.existsSync);

const f = v => +v.toFixed(2);
const P = (cx, cy, r, a) => `${f(cx + r * Math.cos(a))} ${f(cy + r * Math.sin(a))}`;
const D = Math.PI / 180;

// ---------------------------------------------------------------- piezas comunes
// rectangulo de esquinas continuas de iOS (curvatura sin saltos: la esquina empieza a 1,528 r del vertice,
// tres Bezier por esquina, constantes de la mascara de iOS 7+); r = 22,37 % del lado
function squircle(w = 100, r = 22.37) {
  // esquina superior derecha en coordenadas locales (u hacia dentro desde el borde derecho, v hacia abajo)
  const C = [[1.08849323, 0, .86840689, .02196619, .63149399, .07491100],
             [.37282392, .16905732, .16905732, .37282392, .07491100, .63149399],
             [.02196619, .86840689, 0, 1.08849323, 0, 1.52866483]];
  const rot = [(u, v) => [w - u * r, v * r], (u, v) => [w - v * r, w - u * r], (u, v) => [u * r, w - v * r], (u, v) => [v * r, u * r]];
  let d = `M${f(1.52866483 * r)} 0`;
  for (const T of rot) {
    const [sx, sy] = T(1.52866483, 0); d += `L${f(sx)} ${f(sy)}`;
    for (const c of C) { const p = [T(c[0], c[1]), T(c[2], c[3]), T(c[4], c[5])]; d += 'C' + p.map(([x, y]) => f(x) + ' ' + f(y)).join(' '); }
  }
  return d + 'Z';
}
const SQ = squircle();
// degradados: stops = [[offset, color, opacidad?], ...]
const stops = s => s.map(([o, c, a = 1]) => `<stop offset="${o}" stop-color="${c}" stop-opacity="${a}"/>`).join('');
const lin = (id, s, x1 = 0, y1 = 0, x2 = 0, y2 = 1) => `<linearGradient id="${id}" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}">${stops(s)}</linearGradient>`;
const linU = (id, s, x1, y1, x2, y2) => `<linearGradient id="${id}" gradientUnits="userSpaceOnUse" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}">${stops(s)}</linearGradient>`;
const rad = (id, s, cx = .5, cy = .5, r = .5, fx = cx, fy = cy) => `<radialGradient id="${id}" cx="${cx}" cy="${cy}" r="${r}" fx="${fx}" fy="${fy}">${stops(s)}</radialGradient>`;
// sombra hacia abajo (luz cenital); unidades del lienzo de 100
const shadow = (id, dy = 1.6, sd = 1.6, op = .28, col = '#000') =>
  `<filter id="${id}" x="-30%" y="-30%" width="160%" height="160%"><feDropShadow dx="0" dy="${dy}" stdDeviation="${sd}" flood-color="${col}" flood-opacity="${op}"/></filter>`;
const blur = (id, sd) => `<filter id="${id}" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="${sd}"/></filter>`;
// degradado conico (SVG no lo tiene): cunas solapadas; col(t) -> [color, opacidad], t en [0,1)
function conic(cx, cy, r, col, n = 90) {
  let s = '';
  for (let i = 0; i < n; i++) {
    const a0 = (i / n) * 2 * Math.PI - Math.PI / 2, a1 = ((i + 1.6) / n) * 2 * Math.PI - Math.PI / 2;
    const [c, o] = col((i + .5) / n);
    s += `<path d="M${cx} ${cy}L${P(cx, cy, r, a0)}A${r} ${r} 0 0 1 ${P(cx, cy, r, a1)}Z" fill="${c}" fill-opacity="${o}"/>`;
  }
  return s;
}
// engranaje: n dientes trapezoidales entre r (base) y R (punta)
function gearPath(cx, cy, R, r, n, tip = .30, base = .52, rot = 0) {
  const p = 2 * Math.PI / n; let d = '';
  for (let i = 0; i < n; i++) {
    const a = rot + i * p, hb = p * base / 2, ht = p * tip / 2;
    d += (i ? 'L' : 'M') + P(cx, cy, r, a - hb) + 'L' + P(cx, cy, R, a - ht) + 'L' + P(cx, cy, R, a + ht) + 'L' + P(cx, cy, r, a + hb)
      + `A${r} ${r} 0 0 1 ` + P(cx, cy, r, a + p - hb);
  }
  return d + 'Z';
}
const ring = (cx, cy, R, r) => `M${cx - R} ${cy}a${R} ${R} 0 1 0 ${2 * R} 0a${R} ${R} 0 1 0 ${-2 * R} 0ZM${cx - r} ${cy}a${r} ${r} 0 1 1 ${2 * r} 0a${r} ${r} 0 1 1 ${-2 * r} 0Z`;

// lienzo del icono: fondo + arte recortados por la mascara + filo de luz superior
function tile(size, bg, defs, art, { light = false } = {}) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 100 100">
  <defs><clipPath id="m"><path d="${SQ}"/></clipPath>${lin('bg', bg)}
    ${lin('rimT', [[0, '#fff', light ? .9 : .38], [.22, '#fff', 0]])}
    ${lin('rimB', [[.75, '#000', 0], [1, '#000', light ? .16 : .14]])}${defs}</defs>
  <g clip-path="url(#m)">
    <rect width="100" height="100" fill="url(#bg)"/>
    ${art}
    <path d="${SQ}" fill="none" stroke="url(#rimT)" stroke-width="2.2"/>
    <path d="${SQ}" fill="none" stroke="url(#rimB)" stroke-width="2.2"/>
    ${light ? `<path d="${SQ}" fill="none" stroke="#000" stroke-opacity=".10" stroke-width="1.6"/>` : ''}
  </g></svg>`;
}

// ---------------------------------------------------------------- iconos
const ICONS = {};

// Musica: corcheas unidas sobre degradado rosa-rojo vivo
ICONS.music = s => tile(s, [[0, '#ff6a88'], [1, '#f9203f']], shadow('sh', 1.4, 1.5, .22, '#7a0018'),
  // centrada: la caja del dibujo (x 25,5-71, y 23,3-75,9) al centro del icono, 0,3 mas arriba
  // por el peso de las cabezas; en x, centrado optico entre la caja y la masa (que cae a la
  // derecha por las plicas): antes iba 3,5 a la derecha y se veia descentrada
  `<g filter="url(#sh)" fill="#fff" transform="translate(0 0.15)">
     <path d="M38.5 33.2c0-1.6 1.1-2.9 2.6-3.3l26.2-6.6c1.9-.5 3.7.9 3.7 2.9v38.2h-5.4V36.1l-21.7 5.4v26.8h-5.4z"/>
     <ellipse cx="34.6" cy="68.3" rx="9.4" ry="7.2" transform="rotate(-22 34.6 68.3)"/>
     <ellipse cx="61.6" cy="64.4" rx="9.4" ry="7.2" transform="rotate(-22 61.6 64.4)"/>
   </g>`);

// Ajustes: engranaje metalico con dientes, aro claro, rueda de radios y cubo
ICONS.settings = s => {
  let spokes = '';
  for (let i = 0; i < 6; i++) {
    const a = i * 60 * D - 90 * D, w0 = 9 * D, w1 = 4.2 * D;
    spokes += `<path d="M${P(50, 50, 9, a - w0 * 2.2)}L${P(50, 50, 24, a - w1)}L${P(50, 50, 24, a + w1)}L${P(50, 50, 9, a + w0 * 2.2)}Z"/>`;
  }
  return tile(s, [[0, '#e9e9ee'], [1, '#8e8e94']],
    lin('gd', [[0, '#6c6c72'], [1, '#232326']]) + lin('mt', [[0, '#ffffff'], [.5, '#d4d4da'], [1, '#9a9aa1']], 0, 0, .3, 1)
    + lin('mt2', [[0, '#f4f4f7'], [1, '#a9a9b0']]) + lin('dk', [[0, '#1c1c1f'], [1, '#3e3e43']])
    + shadow('sh', 1.2, 1.4, .35) + shadow('sh2', .8, .7, .45),
    `<g filter="url(#sh)">
       <path d="${gearPath(50, 50, 40.5, 34.5, 22, .34, .56, -90 * D)}" fill="url(#gd)" stroke="url(#gd)" stroke-width="1.2" stroke-linejoin="round"/>
     </g>
     <circle cx="50" cy="50" r="31" fill="url(#mt)"/>
     <circle cx="50" cy="50" r="31" fill="none" stroke="#fff" stroke-opacity=".7" stroke-width=".7"/>
     <circle cx="50" cy="50" r="25.5" fill="url(#dk)"/>
     <circle cx="50" cy="50" r="25.5" fill="none" stroke="#000" stroke-opacity=".35" stroke-width=".8"/>
     <g fill="url(#mt2)" filter="url(#sh2)">${spokes}<circle cx="50" cy="50" r="10"/></g>
     <circle cx="50" cy="50" r="4" fill="#3a3a3f"/>
     <circle cx="50" cy="50" r="4" fill="none" stroke="#000" stroke-opacity=".3" stroke-width=".6"/>`);
};

// Inalambrico: abanico Wi-Fi + insignia Bluetooth en un circulo blanco
ICONS.wireless = s => {
  const cx = 45, cy = 63;
  const arc = r => `M${P(cx, cy, r, -135 * D)}A${r} ${r} 0 0 1 ${P(cx, cy, r, -45 * D)}`;
  return tile(s, [[0, '#5fe07e'], [1, '#1fa344']], shadow('sh', 1.3, 1.4, .25, '#00401a') + lin('bb', [[0, '#ffffff'], [1, '#e8ecf2']]),
    `<g filter="url(#sh)">
       <g fill="none" stroke="#fff" stroke-width="7.4" stroke-linecap="round">
         <path d="${arc(14.5)}"/><path d="${arc(25)}"/><path d="${arc(35.5)}"/>
       </g>
       <circle cx="${cx}" cy="${cy}" r="5.4" fill="#fff"/>
     </g>
     <circle cx="70.5" cy="70.5" r="17.2" fill="#22a849"/>
     <g filter="url(#sh)"><circle cx="70.5" cy="70.5" r="14.6" fill="url(#bb)"/></g>
     <path d="M64.8 63.8l11.4 11.2-5.7 5.6V60.4l5.7 5.6-11.4 11.2" fill="none" stroke="#0a7cff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>`);
};

// Libros: libro abierto con paginas curvadas, sombra en el lomo y canto de paginas
ICONS.books = s => tile(s, [[0, '#ffb84a'], [1, '#ff7b00']],
  lin('pl', [[0, '#ffffff'], [.82, '#fbf6ef'], [1, '#e9dccb']], 0, 0, 1, 0) + lin('pr', [[0, '#e9dccb'], [.18, '#fbf6ef'], [1, '#ffffff']], 0, 0, 1, 0)
  + shadow('sh', 1.8, 1.8, .3, '#7a3200'),
  `<g filter="url(#sh)">
     <path d="M17 33.5C27 29.5 39 29.5 50 35.5 61 29.5 73 29.5 83 33.5V74.5C73 70.5 61 71 50 76.5 39 71 27 70.5 17 74.5Z" fill="#c8630b"/>
     <path d="M19 31.5C29 27.5 40 28 50 33.5V73.5C40 68.3 29 68 19 71.8Z" fill="#efe4d6"/>
     <path d="M81 31.5C71 27.5 60 28 50 33.5V73.5C60 68.3 71 68 81 71.8Z" fill="#efe4d6"/>
     <path d="M20 29C30 25 40.5 25.8 50 31.3V71C40.5 65.8 30 65.4 20 69.3Z" fill="url(#pl)"/>
     <path d="M80 29C70 25 59.5 25.8 50 31.3V71C59.5 65.8 70 65.4 80 69.3Z" fill="url(#pr)"/>
   </g>
   <g fill="none" stroke="#ff8a1e" stroke-opacity=".38" stroke-width="1.5" stroke-linecap="round">
     <path d="M26 37.2c6-1.6 11.6-1.2 17.5 1.2M26 44.2c6-1.6 11.6-1.2 17.5 1.2M26 51.2c6-1.6 11.6-1.2 17.5 1.2M26 58.2c4.4-1.2 8.4-1 12 .3"/>
     <path d="M74 37.2c-6-1.6-11.6-1.2-17.5 1.2M74 44.2c-6-1.6-11.6-1.2-17.5 1.2M74 51.2c-6-1.6-11.6-1.2-17.5 1.2M74 58.2c-4.4-1.2-8.4-1-12 .3"/>
   </g>
   <path d="M50 31.3V71" stroke="#c9b9a4" stroke-width=".9"/>`);

// DAC USB: caja de aluminio con pantalla (espectro), rueda de volumen, RCA rojo/blanco y puerto USB-C
ICONS.dac = s => {
  const bars = [8, 13, 10, 15, 9, 12, 6].map((h, i) => `<rect x="${24.6 + i * 3.9}" y="${53 - h}" width="2.5" height="${h}" rx=".8"/>`).join('');
  return tile(s, [[0, '#8583ff'], [1, '#3f3cc8']],
    lin('al', [[0, '#ffffff'], [.55, '#e4e4ea'], [1, '#b9b9c1']]) + lin('ed', [[0, '#8a8a93'], [1, '#5c5c64']])
    + lin('lcd', [[0, '#1d1d24'], [1, '#09090c']]) + lin('bar', [[0, '#7cf2ff'], [1, '#18a8ff']])
    + rad('kn', [[0, '#ffffff'], [.6, '#dcdce2'], [1, '#8e8e96']], .4, .3, .75) + shadow('sh', 2.2, 2.2, .38, '#14125a')
    + rad('jr', [[0, '#3a0a0a'], [.45, '#3a0a0a'], [.5, '#ff453a'], [1, '#b3150c']]) + rad('jw', [[0, '#202024'], [.45, '#202024'], [.5, '#ffffff'], [1, '#b8b8c0']]),
    `<g filter="url(#sh)"><rect x="14" y="28.5" width="72" height="45" rx="8" fill="url(#ed)"/></g>
     <rect x="14" y="27" width="72" height="44" rx="8" fill="url(#al)"/>
     <rect x="14.4" y="27.4" width="71.2" height="43.2" rx="7.6" fill="none" stroke="#fff" stroke-width=".8"/>
     <rect x="21.5" y="34" width="32" height="22" rx="3.5" fill="url(#lcd)"/>
     <g fill="url(#bar)">${bars}</g>
     <rect x="21.5" y="34" width="32" height="22" rx="3.5" fill="none" stroke="#000" stroke-opacity=".35" stroke-width=".6"/>
     <circle cx="26.2" cy="63.5" r="3.6" fill="url(#jr)"/><circle cx="35.6" cy="63.5" r="3.6" fill="url(#jw)"/>
     <rect x="43" y="61.7" width="10.5" height="3.8" rx="1.9" fill="#1a1a1f"/><rect x="45" y="63.1" width="6.5" height="1" rx=".5" fill="#6a6a72"/>
     <circle cx="69.5" cy="49" r="11.6" fill="#8e8e96" fill-opacity=".55"/>
     <circle cx="69.5" cy="48.4" r="10.6" fill="url(#kn)"/>
     <circle cx="69.5" cy="48.4" r="10.6" fill="none" stroke="#000" stroke-opacity=".18" stroke-width=".6"/>
     <circle cx="69.5" cy="48.4" r="7.2" fill="none" stroke="#000" stroke-opacity=".07" stroke-width="1.2"/>
     <circle cx="${f(69.5 + 6.6 * Math.cos(-125 * D))}" cy="${f(48.4 + 6.6 * Math.sin(-125 * D))}" r="1.6" fill="#ff7a1a"/>`);
};

// Streaming (etiqueta "Tidal" en la pantalla de inicio): tres ondas fluidas en cian; NO es el logotipo de Tidal
ICONS.tidal = s => {
  const wave = (y, amp, ph) => { let d = ''; for (let x = 14; x <= 86.01; x += 1.5) d += (x === 14 ? 'M' : 'L') + f(x) + ' ' + f(y + amp * Math.sin((x - 14) / 72 * 2 * Math.PI * 1.15 + ph)); return d; };
  return tile(s, [[0, '#2c2c30'], [1, '#050506']],
    linU('w1', [[0, '#5ac8fa', 0], [.25, '#5ac8fa', .95], [.75, '#9ef0ff', .95], [1, '#9ef0ff', 0]], 14, 0, 86, 0)
    + linU('w2', [[0, '#ffffff', 0], [.3, '#ffffff', 1], [.7, '#ffffff', 1], [1, '#ffffff', 0]], 14, 0, 86, 0)
    + linU('w3', [[0, '#0a84ff', 0], [.25, '#2f9bff', .9], [.75, '#5ac8fa', .9], [1, '#5ac8fa', 0]], 14, 0, 86, 0) + blur('gl', 2.4),
    `<g fill="none" stroke-linecap="round" stroke-width="6">
       <g filter="url(#gl)" opacity=".3"><path d="${wave(50, 7, .5)}" stroke="url(#w2)"/></g>
       <path d="${wave(36, 6, 1.4)}" stroke="url(#w1)" stroke-width="5"/>
       <path d="${wave(50, 7, .5)}" stroke="url(#w2)"/>
       <path d="${wave(64, 6, -.4)}" stroke="url(#w3)" stroke-width="5"/>
     </g>`);
};

// Streaming (etiqueta "Qobuz"): disco compacto iridiscente sobre azul; NO es el logotipo de Qobuz
ICONS.qobuz = s => {
  const hue = t => { const h = [[0, '#ffd1e8'], [.17, '#fff3c4'], [.33, '#c9fff1'], [.5, '#cfe0ff'], [.67, '#ead1ff'], [.83, '#ffd8cf'], [1, '#ffd1e8']];
    let i = 0; while (h[i + 1][0] < t) i++; return h[(t - h[i][0]) / (h[i + 1][0] - h[i][0]) < .5 ? i : i + 1][1]; };
  return tile(s, [[0, '#3f7dff'], [1, '#0b2a96']],
    `<mask id="hole"><rect width="100" height="100" fill="#fff"/><circle cx="50" cy="50" r="5.2" fill="#000"/></mask>`
    + blur('bl', 1.6) + shadow('sh', 2, 2.2, .4, '#000a40') + rad('hub', [[0, '#ffffff', .5], [1, '#d8dde8', .85]])
    + lin('spec', [[0, '#fff', .0], [.5, '#fff', .9], [1, '#fff', 0]], 0, 0, 1, 0),
    `<g mask="url(#hole)">
       <g filter="url(#sh)"><circle cx="50" cy="50" r="33" fill="#c9ced8"/></g>
       <clipPath id="cd"><circle cx="50" cy="50" r="33"/></clipPath>
       <g clip-path="url(#cd)">
         <g filter="url(#bl)">${conic(50, 50, 36, t => [hue((t * 2) % 1), 1])}</g>
         <g filter="url(#bl)" opacity=".85">${conic(50, 50, 36, t => ['#ffffff', Math.max(0, Math.cos((t - .125) * 4 * Math.PI)) ** 6 * .95])}</g>
       </g>
       <circle cx="50" cy="50" r="32.6" fill="none" stroke="#fff" stroke-opacity=".9" stroke-width=".8"/>
       <circle cx="50" cy="50" r="13.5" fill="url(#hub)"/>
       <circle cx="50" cy="50" r="13.5" fill="none" stroke="#8a93a8" stroke-opacity=".5" stroke-width=".6"/>
       <circle cx="50" cy="50" r="9" fill="none" stroke="#fff" stroke-opacity=".9" stroke-width="1"/>
     </g>`);
};

// Reproduciendo: caratula con paisaje y sol + forma de onda
ICONS.nowplaying = s => {
  const hs = [7, 13, 19, 11, 16, 9, 5];
  const bars = hs.map((h, i) => `<line x1="${27.5 + i * 7.5}" y1="${77 - h / 2}" x2="${27.5 + i * 7.5}" y2="${77 + h / 2}"/>`).join('');
  return tile(s, [[0, '#ffa31a'], [1, '#ff3366']],
    lin('art', [[0, '#5b5cf0'], [1, '#c45cf2']]) + lin('hill', [[0, '#ff7aa0'], [1, '#ff4f7b']]) + lin('hill2', [[0, '#ffb15c'], [1, '#ff7f50']])
    + rad('sun', [[0, '#fff6b0'], [1, '#ffd60a']]) + shadow('sh', 1.8, 2, .32, '#6e0024') + shadow('sh2', 1, 1, .2, '#6e0024'),
    `<clipPath id="ac"><rect x="28" y="14" width="44" height="44" rx="6.5"/></clipPath>
     <g filter="url(#sh)"><rect x="28" y="14" width="44" height="44" rx="6.5" fill="url(#art)"/></g>
     <g clip-path="url(#ac)">
       <circle cx="60" cy="28" r="7" fill="url(#sun)"/>
       <path d="M24 50C32 40 40 38 49 45S63 46 76 36V62H24Z" fill="url(#hill)"/>
       <path d="M24 56C35 47 46 49 56 53S68 52 76 48V62H24Z" fill="url(#hill2)"/>
       <rect x="28" y="14" width="44" height="22" fill="#fff" fill-opacity=".08"/>
     </g>
     <rect x="28.3" y="14.3" width="43.4" height="43.4" rx="6.2" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width=".6"/>
     <g stroke="#fff" stroke-width="4.2" stroke-linecap="round" filter="url(#sh2)">${bars}</g>`);
};

// Radio: receptor retro con altavoz, dial con aguja, antena y ondas
ICONS.radio = s => tile(s, [[0, '#cf8cff'], [1, '#7d34e0']],
  lin('bd', [[0, '#ffffff'], [1, '#e6dcf6']]) + rad('spk', [[0, '#5a4890'], [1, '#2c2152']]) + lin('dl', [[0, '#2a1f48'], [1, '#160f2c']])
  + shadow('sh', 2, 2.2, .35, '#33006e'),
  `<g fill="none" stroke="#fff" stroke-linecap="round">
     <path d="M62 41L76 19.5" stroke-width="2.8"/>
     <path d="M70.8 12.6a9 9 0 0 1 11.6 3.6" stroke-width="2.6" stroke-opacity=".85"/>
     <path d="M68.6 6.4a15.5 15.5 0 0 1 19.8 6.4" stroke-width="2.6" stroke-opacity=".5"/>
   </g>
   <circle cx="76.4" cy="19" r="2.8" fill="#fff"/>
   <g filter="url(#sh)"><rect x="15" y="39" width="70" height="42" rx="9" fill="url(#bd)"/></g>
   <rect x="15.4" y="39.4" width="69.2" height="41.2" rx="8.6" fill="none" stroke="#fff" stroke-width=".8"/>
   <circle cx="35" cy="60" r="13" fill="url(#spk)"/>
   <g fill="none" stroke="#8f7ad0" stroke-opacity=".7" stroke-width="1"><circle cx="35" cy="60" r="9.6"/><circle cx="35" cy="60" r="6.2"/><circle cx="35" cy="60" r="2.8"/></g>
   <circle cx="35" cy="60" r="13" fill="none" stroke="#000" stroke-opacity=".25" stroke-width=".6"/>
   <rect x="54" y="47" width="24" height="11" rx="2.6" fill="url(#dl)"/>
   <g stroke="#fff" stroke-opacity=".75" stroke-width=".9"><path d="M57.5 50v5M61.5 51.5v3.5M65.5 50v5M69.5 51.5v3.5M73.5 50v5"/></g>
   <path d="M67 48.5v8" stroke="#ff453a" stroke-width="1.6" stroke-linecap="round"/>
   <circle cx="60" cy="69" r="3.6" fill="#b9aed6"/><circle cx="72" cy="69" r="3.6" fill="#b9aed6"/>
   <circle cx="60" cy="68.4" r="3" fill="#f4f0fb"/><circle cx="72" cy="68.4" r="3" fill="#f4f0fb"/>`);

// Personalizar: tres deslizadores verticales con relleno de color y pomos blancos
ICONS.personalize = s => {
  const sl = [[31, 46, 'a'], [50, 64, 'b'], [69, 34, 'c']];
  return tile(s, [[0, '#5c5c62'], [1, '#1f1f22']],
    lin('a', [[0, '#ff6b9a'], [1, '#ff2d55']]) + lin('b', [[0, '#ffe066'], [1, '#ffb800']]) + lin('c', [[0, '#7ee7ff'], [1, '#30a8ff']])
    + lin('kb', [[0, '#ffffff'], [1, '#dedee4']]) + shadow('sh', 1.4, 1.5, .45),
    sl.map(([x, y, g]) => `
     <rect x="${x - 3.8}" y="20" width="7.6" height="60" rx="3.8" fill="#000" fill-opacity=".35"/>
     <rect x="${x - 3.8}" y="20" width="7.6" height="60" rx="3.8" fill="none" stroke="#fff" stroke-opacity=".08" stroke-width=".6"/>
     <rect x="${x - 3.8}" y="${y}" width="7.6" height="${80 - y}" rx="3.8" fill="url(#${g})"/>
     <g filter="url(#sh)"><circle cx="${x}" cy="${y}" r="7.6" fill="url(#kb)"/></g>`).join(''));
};

// Apps: marca propia (rejilla 2x2 con un "+"), NO la "A" de la App Store de Apple
ICONS.appstore = s => tile(s, [[0, '#2ad0ff'], [1, '#1663ee']],
  lin('t', [[0, '#ffffff'], [1, '#e6f0ff']]) + shadow('sh', 1.5, 1.6, .3, '#00287a'),
  `<g filter="url(#sh)" fill="url(#t)">
     <rect x="24" y="24" width="23" height="23" rx="6.5"/><rect x="53" y="24" width="23" height="23" rx="6.5"/>
     <rect x="24" y="53" width="23" height="23" rx="6.5"/>
     <rect x="61.6" y="52.5" width="5.8" height="24" rx="2.9"/><rect x="52.5" y="61.6" width="24" height="5.8" rx="2.9"/>
   </g>`);

// Auriculares: diadema metalica, almohadillas y conchas grafito sobre turquesa
ICONS.headphones = s => tile(s, [[0, '#5fe6d4'], [1, '#0a9e93']],
  lin('hb', [[0, '#5a5a60'], [1, '#2a2a2e']]) + lin('cup', [[0, '#4a4a50'], [1, '#1c1c1f']], 0, 0, .4, 1)
  + lin('pad', [[0, '#77777e'], [1, '#3c3c42']]) + lin('sl', [[0, '#f2f2f6'], [1, '#a2a2aa']]) + shadow('sh', 1.8, 2, .35, '#00413b'),
  `<g filter="url(#sh)">
     <path d="M21 60V52a29 29 0 0 1 58 0v8" fill="none" stroke="url(#hb)" stroke-width="6.4" stroke-linecap="round"/>
     <path d="M27.5 53a22.5 22.5 0 0 1 45 0" fill="none" stroke="#fff" stroke-opacity=".18" stroke-width="1.2"/>
     <rect x="18.6" y="55" width="4.8" height="9" rx="1.6" fill="url(#sl)"/><rect x="76.6" y="55" width="4.8" height="9" rx="1.6" fill="url(#sl)"/>
     <rect x="13" y="58" width="17" height="27" rx="7.5" fill="url(#cup)"/>
     <rect x="70" y="58" width="17" height="27" rx="7.5" fill="url(#cup)"/>
     <rect x="25.5" y="60.5" width="8" height="22" rx="4" fill="url(#pad)"/>
     <rect x="66.5" y="60.5" width="8" height="22" rx="4" fill="url(#pad)"/>
   </g>
   <path d="M15.5 64a6 6 0 0 1 5-4.5" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="1.2" stroke-linecap="round"/>
   <path d="M72.5 64a6 6 0 0 1 5-4.5" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="1.2" stroke-linecap="round"/>`);

// Archivos: carpeta azul (fondo con pestana, hoja asomando y frontal) sobre blanco
ICONS.files = s => tile(s, [[0, '#ffffff'], [1, '#dfe2e8']],
  lin('bk', [[0, '#2a8ff7'], [1, '#0f62d8']]) + lin('fr', [[0, '#6fd0ff'], [.5, '#3aa6ff'], [1, '#1e7ef5']])
  + lin('pp', [[0, '#ffffff'], [1, '#e9edf3']]) + shadow('sh', 1.8, 2, .22, '#0a2a66') + shadow('sh2', -.6, 1, .18, '#0a2a66'),
  `<g filter="url(#sh)">
     <path d="M15 31a5 5 0 0 1 5-5h14.4a5 5 0 0 1 3.7 1.6l3.4 3.6H80a5 5 0 0 1 5 5V73a5 5 0 0 1-5 5H20a5 5 0 0 1-5-5Z" fill="url(#bk)"/>
   </g>
   <rect x="21" y="34" width="58" height="22" rx="2.5" fill="url(#pp)"/>
   <g filter="url(#sh2)"><rect x="15" y="40.5" width="70" height="37.5" rx="5" fill="url(#fr)"/></g>
   <rect x="15.4" y="40.9" width="69.2" height="36.7" rx="4.6" fill="none" stroke="#fff" stroke-opacity=".5" stroke-width=".8"/>`, { light: true });

// Gutenberg (audiolibros y libros libres): prensa de imprenta de madera en crema sobre rojo sangre de buey;
// cabezal, montantes, husillo roscado con barra, platina sobre la hoja de papel, cama y pies
ICONS.gutenberg = s => {
  let thread = '';
  for (let y = 28.5; y < 44; y += 3) thread += `M46.6 ${f(y + 1.6)}L53.4 ${f(y)}`;
  return tile(s, [[0, '#b5483a'], [1, '#5e1712']],
    lin('wd', [[0, '#fff8e8'], [1, '#e9d5ad']]) + lin('wd2', [[0, '#f3e4c4'], [1, '#d4b98a']], 0, 0, 1, 0)
    + lin('sc', [[0, '#c9ab78'], [.5, '#fff4d8'], [1, '#b8955e']], 0, 0, 1, 0) + lin('pp', [[0, '#ffffff'], [1, '#f1ece4']])
    + shadow('sh', 1.8, 1.8, .38, '#2a0503'),
    `<g filter="url(#sh)">
       <path d="M54 34.6L84.5 29" fill="none" stroke="url(#wd)" stroke-width="3.4" stroke-linecap="round"/>
       <circle cx="85" cy="28.9" r="3.6" fill="url(#wd)"/>
       <rect x="22" y="20" width="8.5" height="62" rx="2" fill="url(#wd2)"/>
       <rect x="69.5" y="20" width="8.5" height="62" rx="2" fill="url(#wd2)"/>
       <rect x="17" y="15" width="66" height="10.5" rx="3" fill="url(#wd)"/>
       <rect x="46.6" y="25" width="6.8" height="20" fill="url(#sc)"/>
       <rect x="43.5" y="31.6" width="13" height="5.6" rx="2" fill="url(#wd)"/>
       <rect x="34" y="44.5" width="32" height="8" rx="2" fill="url(#wd)"/>
       <rect x="31" y="54.6" width="38" height="4.6" rx=".8" fill="url(#pp)"/>
       <rect x="17" y="59" width="66" height="9" rx="2.4" fill="url(#wd)"/>
       <rect x="12" y="78" width="76" height="7" rx="3" fill="url(#wd)"/>
     </g>
     <path d="${thread}" stroke="#8d6a3a" stroke-opacity=".7" stroke-width="1.1" stroke-linecap="round"/>
     <path d="M17.8 25.2h64.4M34.6 52.3h30.8M17.8 67.8h64.4M12.8 84.8h74.4" stroke="#8d6a3a" stroke-opacity=".35" stroke-width=".8"/>
     <path d="M35 56.3h18M35 57.8h24" stroke="#5e1712" stroke-opacity=".45" stroke-width=".7"/>`);
};

// Vinilo (Free Music): disco con surcos, brillo conico, etiqueta de color, eje y brazo del tocadiscos
ICONS.freemusic = s => {
  let grooves = '';
  for (let r = 15; r <= 32.6; r += 1.6) grooves += `<circle cx="50" cy="50" r="${f(r)}" fill="none" stroke="#fff" stroke-opacity="${(Math.round(r / 1.6) % 3) ? .045 : .09}" stroke-width=".55"/>`;
  return tile(s, [[0, '#3c3c41'], [1, '#101012']],
    rad('v1', [[.4, '#232327'], [.96, '#0b0b0d'], [1, '#26262a']]) + lin('lb', [[0, '#ff5f6d'], [1, '#ffb347']], 0, 0, 1, 1)
    + blur('bl', 1.8) + shadow('sh', 2, 2.4, .5) + lin('arm', [[0, '#ffffff'], [1, '#9c9ca4']], 0, 0, 1, 1) + shadow('sh2', 1.2, 1, .5)
    + rad('pv', [[0, '#ffffff'], [.7, '#c7c7cc'], [1, '#7c7c84']], .4, .35, .7),
    `<g filter="url(#sh)"><circle cx="47" cy="52" r="34" fill="url(#v1)"/></g>
     <clipPath id="dc"><circle cx="47" cy="52" r="34"/></clipPath>
     <g transform="translate(-3 2)">${grooves}</g>
     <g clip-path="url(#dc)" filter="url(#bl)">${conic(47, 52, 36, t => ['#ffffff', Math.max(0, Math.cos((t - .1) * 4 * Math.PI)) ** 8 * .26])}</g>
     <circle cx="47" cy="52" r="33.7" fill="none" stroke="#fff" stroke-opacity=".14" stroke-width=".6"/>
     <circle cx="47" cy="52" r="12" fill="url(#lb)"/>
     <circle cx="47" cy="52" r="12" fill="none" stroke="#000" stroke-opacity=".25" stroke-width=".6"/>
     <circle cx="47" cy="52" r="8.6" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width=".6"/>
     <circle cx="47" cy="52" r="2.2" fill="#e5e5ea"/><circle cx="47" cy="52" r="1" fill="#6e6e73"/>
     <g filter="url(#sh2)">
       <path d="M80 17L75.5 43 63.5 57.5" fill="none" stroke="url(#arm)" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/>
       <rect x="58.5" y="55" width="8.5" height="5.5" rx="1.4" transform="rotate(40 62.7 57.7)" fill="#d8d8de"/>
       <circle cx="80" cy="17" r="6" fill="url(#pv)"/><circle cx="80" cy="17" r="2.2" fill="#55555c"/>
     </g>`);
};

// ---------------------------------------------------------------- glifos del Centro de control (26x26, peso tipo SF Symbols "semibold")
const W2 = 2.2;   // trazo comun
const st = (d, w = W2) => `<path d="${d}" fill="none" stroke="#fff" stroke-width="${w}" stroke-linecap="round" stroke-linejoin="round"/>`;
const fl = d => `<path d="${d}" fill="#fff" stroke="#fff" stroke-width="1" stroke-linejoin="round"/>`;
const CC = {
  bolt: fl('M14.8 2.6L6.6 14.3c-.3.5 0 1 .6 1H12l-1.6 7.9c-.1.6.6.9 1 .4l8.1-11.6c.3-.5 0-1-.6-1H14l1.6-7.9c.1-.6-.6-.9-.8-.5z'),
  folder: fl('M3.5 7.5a2 2 0 0 1 2-2h4.6c.6 0 1.1.2 1.5.6l1.4 1.4h7.5a2 2 0 0 1 2 2v2H3.5z')
    + `<rect x="3" y="10.6" width="20" height="10.6" rx="2.2" fill="#fff"/>`,
  gauge: st('M6.1 19.6a9.2 9.2 0 1 1 13.8 0') + st('M13 14.6l4.2-4.4', 2.1) + `<circle cx="13" cy="14.6" r="2.3" fill="#fff"/>`
    + `<g fill="#fff"><circle cx="8.4" cy="10.2" r="1"/><circle cx="13" cy="8.2" r="1"/><circle cx="7" cy="15.2" r="1"/><circle cx="19" cy="15.2" r="1"/></g>`,
  gear: `<path fill="#fff" fill-rule="evenodd" stroke="#fff" stroke-width="1.1" stroke-linejoin="round" d="${gearPath(13, 13, 10.6, 8, 8, .36, .6, -90 * D)}${'M16.4 13a3.4 3.4 0 1 0-6.8 0 3.4 3.4 0 1 0 6.8 0Z'}"/>`,
  leaf: `<mask id="lv"><rect width="26" height="26" fill="#fff"/><path d="M8.6 19.2C11.5 14.5 14.5 11.6 18.4 8.6" stroke="#000" stroke-width="1.6" stroke-linecap="round" fill="none"/></mask>`
    + `<path mask="url(#lv)" fill="#fff" d="M21.4 3.6C13 3.6 6.4 6.8 6 14.2c-.2 3.6 2.4 6.2 6 6 7.4-.4 9.6-7.6 9.4-16.6z"/>` + st('M4 22.6l5.2-4.8', 2),
  plug: st('M10 3.6v4.6M16 3.6v4.6', 2.3) + fl('M6.6 9.2h12.8v2.6a6.4 6.4 0 0 1-12.8 0z') + st('M13 18.4v4.4', 2.4),
  sliders: `<mask id="sk"><rect width="26" height="26" fill="#fff"/><circle cx="17" cy="7" r="4.6" fill="#000"/><circle cx="9" cy="13" r="4.6" fill="#000"/><circle cx="15" cy="19" r="4.6" fill="#000"/></mask>`
    + `<g mask="url(#sk)">${st('M3.6 7h18.8M3.6 13h18.8M3.6 19h18.8', 2)}</g>` + `<g fill="#fff"><circle cx="17" cy="7" r="3.2"/><circle cx="9" cy="13" r="3.2"/><circle cx="15" cy="19" r="3.2"/></g>`,
  sun: `<circle cx="13" cy="13" r="4.6" fill="#fff"/>` + st(Array.from({ length: 8 }, (_, i) => `M${P(13, 13, 8, i * 45 * D)}L${P(13, 13, 10.6, i * 45 * D)}`).join(''), 2.2),
  // DAC: conector USB-C con cable (generico; no se usa el tridente USB)
  usb: st('M13 17.8v5.4', 2.3) + fl('M8.4 10.4h9.2v5.4a2.4 2.4 0 0 1-2.4 2.4h-4.4a2.4 2.4 0 0 1-2.4-2.4z')
    + `<rect x="9.6" y="2.6" width="6.8" height="8.4" rx="1.8" fill="none" stroke="#fff" stroke-width="1.8"/><rect x="11.9" y="5" width="2.2" height="3.4" rx="1" fill="#fff"/>`,
  // filtro del DAC: respuesta al impulso (sinc)
  wave: st((() => { let d = ''; for (let x = 2.5; x <= 23.51; x += .35) { const u = (x - 13) / 2.1, v = Math.abs(u) < 1e-6 ? 1 : Math.sin(Math.PI * u) / (Math.PI * u); d += (d ? 'L' : 'M') + f(x) + ' ' + f(17.4 - 12.6 * v); } return d; })(), 2.1),
  wifi: `<path d="M13 21.6l-3.3-3.6a4.8 4.8 0 0 1 6.6 0z" fill="#fff" stroke="#fff" stroke-width="1.6" stroke-linejoin="round"/>`
    + st(`M${P(13, 21.4, 8.8, -135 * D)}A8.8 8.8 0 0 1 ${P(13, 21.4, 8.8, -45 * D)}M${P(13, 21.4, 13.6, -135 * D)}A13.6 13.6 0 0 1 ${P(13, 21.4, 13.6, -45 * D)}`, 2.3),
  bt: st('M8.2 8.4l9.4 9-4.6 4.4V4.2l4.6 4.4-9.4 9', 2.35),
  // modo avion: avion visto desde arriba, hacia arriba a la derecha (como el de iOS)
  plane: `<g transform="rotate(45 13 13)">` + fl('M13 2.2c1.1 0 1.9 1.3 1.9 3v4.4l8.3 5.3v2.5l-8.3-2.6v4.7l2.7 2v1.9l-4.6-1.3-4.6 1.3v-1.9l2.7-2v-4.7l-8.3 2.6v-2.5l8.3-5.3V5.2c0-1.7.8-3 1.9-3z') + `</g>`,
  buds: st('M5 17.4v-3a8 8 0 0 1 16 0v3', 2.1)
    + `<rect x="3.4" y="14.6" width="5.4" height="8.6" rx="2.3" fill="#fff"/><rect x="17.2" y="14.6" width="5.4" height="8.6" rx="2.3" fill="#fff"/>`,
};
const ccSvg = (body, z) => `<svg xmlns="http://www.w3.org/2000/svg" width="${z}" height="${z}" viewBox="0 0 26 26">${body}</svg>`;

(async () => {
  const b = await chromium.launch({ executablePath: exe });
  const p = await b.newPage({ deviceScaleFactor: 1 });
  const shot = async (svg, file) => {
    await p.setContent(`<html><body style="margin:0;background:transparent">${svg}</body></html>`);
    await (await p.$('svg')).screenshot({ path: file, omitBackground: true });
  };
  for (const size of SIZES) {
    for (const [name, fn] of Object.entries(ICONS)) await shot(fn(size), path.join(out, `${name}_${size}.png`));
    console.log('tamano', size, 'ok');
  }
  fs.mkdirSync(path.join(out, 'cc'), { recursive: true });
  const z = SIZES.includes(48) ? 26 : SIZES[0] / 2;
  for (const [name, body] of Object.entries(CC)) await shot(ccSvg(body, z), path.join(out, 'cc', `cc_${name}.png`));
  console.log('glifos cc:', Object.keys(CC).length, '(' + z + ' px)');
  await b.close();
})();
