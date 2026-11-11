// Renders theme/bootanim/intro.html to PNG frames (320x480, rendered at 2x and kept at 2x;
// tools/bootanim.py scales them down).
// Usage: node render_intro.js <out_dir> [fps=24] [seconds=7.75]
//   PWCORE = path of playwright-core; Chromium from %LOCALAPPDATA%\ms-playwright.
// Also writes logo.png (the still "ByOS" logo) in <out_dir>.
const { chromium } = require(process.env.PWCORE || 'playwright-core');
const fs = require('fs'), path = require('path');

(async () => {
  const out = process.argv[2], fps = +(process.argv[3] || 24), secs = +(process.argv[4] || 7.75);
  fs.mkdirSync(out, { recursive: true });
  const base = path.join(process.env.LOCALAPPDATA, 'ms-playwright');
  const dir = fs.readdirSync(base).filter(d => d.startsWith('chromium-')).sort().pop();
  const exe = fs.readdirSync(path.join(base, dir)).map(d => path.join(base, dir, d, 'chrome.exe')).find(fs.existsSync);
  const browser = await chromium.launch({ executablePath: exe });
  const page = await browser.newPage({ viewport: { width: 320, height: 480 }, deviceScaleFactor: 2 });
  await page.goto('file://' + path.join(__dirname, 'intro.html').replace(/\\/g, '/'));
  await page.evaluate(() => document.fonts.ready);
  const stage = await page.$('#stage');
  const n = Math.round(fps * secs);
  for (let i = 0; i < n; i++) {
    await page.evaluate(t => window.render(t), i / fps);
    await stage.screenshot({ path: path.join(out, String(i).padStart(4, '0') + '.png') });
  }
  await page.evaluate(() => window.renderLogo());
  await stage.screenshot({ path: path.join(out, 'logo.png'), omitBackground: false });
  await browser.close();
  console.log(n + ' frames in ' + out);
})();
