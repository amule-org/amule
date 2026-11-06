// Run with Node and Playwright installed: node unittests/browser-tests/country-flags.cjs
// Set AMULE_BROWSER_OUTPUT to retain 1x/2x/3x screenshots for visual inspection.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { repoRoot, staticRoot, importMap, serveStatic, launchBrowser, collectPageErrors, run } = require('./lib/harness.cjs');

const flagsRoot = path.join(repoRoot, 'src/icons/flags');
const codes = fs.readdirSync(flagsRoot).filter(name => /^[a-z]{2}\.png$/.test(name))
  .map(name => name.slice(0, 2)).sort();
const html = `<!doctype html><html lang="en"><head><meta charset="utf-8">
<link rel="stylesheet" href="/css/app.css"><script type="importmap">${importMap()}</script>
<style>body{padding:24px}#flags{display:grid;grid-template-columns:repeat(12,1fr);gap:10px}
.country-cell{min-height:24px}</style></head><body><h1>WebUI country flags</h1><div id="flags"></div>
<script type="module">
import { CountryCell } from '/js/components.js';
import { html, render } from '/js/dom.js';
const codes = ${JSON.stringify([...codes, 'zz'])};
render(html\`\${codes.map(code => html\`<div data-code=\${code}><\${CountryCell} code=\${code}/></div>\`)}\`, document.querySelector('#flags'));
window.flagsReady = true;
</script></body></html>`;

run(async () => {
  const server = await serveStatic(staticRoot, {
    mounts: { '/flags/': flagsRoot },
    handler: (req, res, url) => {
      if (url.pathname === '/') {
        res.setHeader('Content-Type', 'text/html');
        res.end(html);
        return true;
      }
      if (/^\/flags\/.*@[23]x\.png$/.test(url.pathname) && req.headers.cookie === 'legacy=1') {
        res.writeHead(404); res.end(); return true;
      }
      return false;
    },
  });
  const browser = await launchBrowser();
  for (const legacy of [false, true]) {
    for (const scale of [1, 2, 3]) {
      const context = await browser.newContext({ viewport: { width: 1200, height: 850 }, deviceScaleFactor: scale });
      if (legacy) await context.addCookies([{ name: 'legacy', value: '1', url: server.origin }]);
      const page = await context.newPage();
      const errors = collectPageErrors(page);
      await page.goto(`${server.origin}/`);
      await page.waitForFunction(() => window.flagsReady);
      await page.waitForFunction(() => [...document.querySelectorAll('img.flag')].every(image => image.complete));
      await page.waitForFunction(({ legacy, scale }) => [...document.querySelectorAll('[data-code]')].every(cell => {
        const image = cell.querySelector('img');
        if (cell.dataset.code === 'zz') return image.style.visibility === 'hidden';
        const density = legacy ? 1 : scale;
        return image.complete && image.naturalWidth > 0 && image.currentSrc.endsWith(
          `${cell.dataset.code}${density === 1 ? '' : '@' + density + 'x'}.png`);
      }), { legacy, scale });
      const images = await page.locator('[data-code]').evaluateAll(cells => cells.map(cell => {
        const image = cell.querySelector('img');
        const rect = image.getBoundingClientRect();
        return { code: cell.dataset.code, src: image.currentSrc, naturalWidth: image.naturalWidth, loaded: image.naturalWidth > 0,
          width: rect.width, height: rect.height, hidden: image.style.visibility === 'hidden' };
      }));
      assert.equal(await page.evaluate(() => devicePixelRatio), scale);
      assert.deepEqual(errors, []);
      assert.equal(images.length, codes.length + 1);
      for (const image of images) {
        assert.equal(image.width, 16, image.code);
        assert.equal(image.height, 12, image.code);
        if (image.code === 'zz') {
          assert.equal(image.hidden, true);
        } else {
          assert.equal(image.loaded, true, image.code);
          assert.equal(image.hidden, false, image.code);
          const density = legacy ? 1 : scale;
          assert.ok(image.src.endsWith(`${image.code}${density === 1 ? '' : '@' + density + 'x'}.png`), image.code);
        }
      }
      if (process.env.AMULE_BROWSER_OUTPUT) {
        fs.mkdirSync(process.env.AMULE_BROWSER_OUTPUT, { recursive: true });
        await page.screenshot({ path: path.join(process.env.AMULE_BROWSER_OUTPUT, `flags-${legacy ? "legacy-" : ""}${scale}x.png`), fullPage: true });
      }
      console.log(`PASS: ${codes.length} WebUI flags (${legacy ? "1x-only API" : "density-aware API"}) at ${scale}x, srcset selection, 1x fallback and missing flag`);
      await context.close();
    }
  }
});
