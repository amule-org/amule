// Run with Node and Playwright installed: node unittests/browser-tests/remember-tabs.cjs
// Set AMULE_BROWSER_EXECUTABLE to use an existing Chromium installation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');
const { setTimeout: delay } = require('node:timers/promises');

const staticRoot = path.resolve(__dirname, '../../src/webapi/static');
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const file = path.resolve(staticRoot, '.' + (url.pathname === '/' ? '/index.html' : url.pathname));
  if (!file.startsWith(staticRoot + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json' };
  res.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
  res.end(fs.readFileSync(file));
});

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ executablePath: process.env.AMULE_BROWSER_EXECUTABLE || undefined });
  try {
    const page = await browser.newPage({ viewport: { width: 1400, height: 1000 } });
    page.setDefaultTimeout(10000);
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    let categories = [{ index: 0, name: 'Default' }, { index: 1, name: 'Books' }];
    let releaseCategories = null;
    await page.route('**/api/v1/**', async route => {
      const endpoint = new URL(route.request().url()).pathname.replace('/api/v1/', '');
      let body = {};
      switch (endpoint) {
        case 'auth/session': body = { role: 'admin' }; break;
        case 'events': await route.fulfill({ contentType: 'text/event-stream', body: ': connected\n\n' }); return;
        case 'categories':
          if (releaseCategories) await new Promise(resolve => { releaseCategories.resolve = resolve; });
          body = { categories }; break;
        case 'search': body = { searches: [
          { search_id: 1, query: 'First query', state: 'finished' },
          { search_id: 2, query: 'Second query', state: 'finished' },
        ] }; break;
        case 'search/1/results': case 'search/2/results': body = { results: [] }; break;
        case 'status': body = { ec_connected: true }; break;
        case 'downloads': body = { downloads: [] }; break;
        case 'servers': body = { servers: [] }; break;
        case 'clients': body = { clients: [] }; break;
        case 'known_clients': body = { known_clients: [] }; break;
        case 'chats': body = { chats: [] }; break;
      }
      await route.fulfill({ json: body });
    });
    await page.goto(`http://127.0.0.1:${server.address().port}/#/networks`);
    const go = async route => {
      await page.locator(`nav a[data-route="${route}"]`).click();
      await page.waitForFunction(route => location.hash === '#/' + route, route);
      const ready = { networks: '.net-split', clients: '.clients-fill', downloads: '.split-view',
                      search: '.search-form', preferences: '.prefs-panel' };
      await page.locator('#view ' + ready[route]).waitFor();
    };
    const tab = name => page.getByRole('tab', { name, exact: true });
    const selected = async locator => {
      await locator.waitFor();
      await page.waitForFunction(el => el.getAttribute('aria-selected') === 'true', await locator.elementHandle());
    };
    await selected(tab('ED2K'));
    await selected(tab('aMule log'));
    await tab('Kad').click();
    await tab('Kad info').click();
    await go('downloads');
    await go('networks');
    await selected(tab('Kad'));
    await selected(tab('Kad info'));
    console.log('PASS: both Networks tab strips survive menu navigation');

    await go('clients');
    const clientTabs = page.getByRole('tablist').first().getByRole('tab');
    await selected(clientTabs.nth(0));
    for (const index of [1, 2, 3]) {
      await clientTabs.nth(index).click();
      await go('networks');
      await go('clients');
      await selected(clientTabs.nth(index));
    }
    console.log('PASS: Downloading, Uploading and Known clients survive menu navigation');

    await go('downloads');
    const books = page.getByRole('tab', { name: /^Books/ });
    await books.click();
    await go('networks');
    releaseCategories = {};
    await go('downloads');
    await page.locator('#view .split-view').waitFor();
    // Hold the response past the mount effects: an empty loading list must
    // not clear the stored category before the real list arrives.
    const deadline = Date.now() + 10000;
    while (!releaseCategories.resolve && Date.now() < deadline) await delay(10);
    assert.ok(releaseCategories.resolve, 'category request is held');
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    releaseCategories.resolve();
    releaseCategories = null;
    await selected(books);
    console.log('PASS: download category survives a delayed category response');

    await go('networks');
    categories = [{ index: 0, name: 'Default' }];
    await go('downloads');
    await selected(page.getByRole('tab', { name: /^All/ }));
    await go('networks');
    categories = [{ index: 0, name: 'Default' }, { index: 1, name: 'Books' }];
    await go('downloads');
    await books.waitFor();
    await selected(page.getByRole('tab', { name: /^All/ }));
    console.log('PASS: a deleted category falls back to All and stays cleared');

    await go('search');
    const first = page.getByRole('tab', { name: /^First query/ });
    await first.click();
    await go('networks');
    await go('search');
    await selected(first);
    console.log('PASS: the existing search registry preserves the selected results tab');

    await go('preferences');
    const prefTabs = page.getByRole('tablist').getByRole('tab');
    await prefTabs.nth(1).click();
    const prefName = await prefTabs.nth(1).textContent();
    await go('networks');
    await go('preferences');
    await selected(tab(prefName));
    console.log('PASS: Preferences remembers its selected tab');

    await go('networks');
    await page.reload();
    await selected(tab('ED2K'));
    await selected(tab('aMule log'));
    assert.deepEqual(errors, []);
    console.log('PASS: a page reload restores defaults; no browser runtime errors');
  } finally {
    await browser.close();
    server.close();
  }
})().catch(error => { console.error(error); server.close(); process.exitCode = 1; });
