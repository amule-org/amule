// Run with Node and Playwright installed: node unittests/browser-tests/remember-tabs.cjs
const assert = require('node:assert/strict');
const { setTimeout: delay } = require('node:timers/promises');
const { serveStatic, launchBrowser, collectPageErrors, run } = require('./lib/harness.cjs');

run(async () => {
  const server = await serveStatic();
  const browser = await launchBrowser();
  const page = await browser.newPage({ locale: 'en-US', viewport: { width: 1400, height: 1000 } });
  page.setDefaultTimeout(10000);
  const errors = collectPageErrors(page);
  let categories = [{ index: 0, name: 'Default' }, { index: 1, name: 'Books' }];
  let releaseCategories = null;
  let categoriesFail = false;
  let geoipSupported = true;
  await page.route('**/api/v1/**', async route => {
    const endpoint = new URL(route.request().url()).pathname.replace('/api/v1/', '');
    let body = {};
    switch (endpoint) {
      case 'auth/session': body = { role: 'admin' }; break;
      case 'events': await route.fulfill({ contentType: 'text/event-stream', body: ': connected\n\n' }); return;
      case 'categories':
        if (categoriesFail) { await route.fulfill({ status: 503, json: { error: { code: 'unavailable' } } }); return; }
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
      case 'preferences': body = { geoip: { supported: geoipSupported } }; break;
    }
    await route.fulfill({ json: body });
  });
  await page.goto(`${server.origin}/#/networks`);
  assert.equal(await page.evaluate(() => navigator.language), 'en-US');
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

  await books.click();
  await go('networks');
  categoriesFail = true;
  await go('downloads');
  await selected(page.getByRole('tab', { name: /^All/ }));
  categoriesFail = false;
  await go('networks');
  await go('downloads');
  await books.waitFor();
  await selected(page.getByRole('tab', { name: /^All/ }));
  console.log('PASS: failed initial category load clears the filter and recovers on return');

  await books.click();
  const manageCategories = page.getByRole('button', { name: 'Manage categories', exact: true });
  await manageCategories.click();
  await page.locator('.cat-panel-header').waitFor();
  await page.locator('.card table').getByText('Books', { exact: true }).waitFor();
  categoriesFail = true;
  const failedRefresh = page.waitForResponse(response =>
    new URL(response.url()).pathname === '/api/v1/categories' && response.status() === 503);
  await manageCategories.click();
  await failedRefresh;
  // Let the rejected request reach its catch and any fallback effect.
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  await selected(books);
  categoriesFail = false;
  console.log('PASS: failed category refresh preserves loaded categories and the selected tab');

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

  await tab('IP2Country').click();
  await go('networks');
  geoipSupported = false;
  await go('preferences');
  await selected(prefTabs.nth(0));
  assert.equal(await tab('IP2Country').count(), 0);
  console.log('PASS: an unavailable Preferences tab highlights the displayed fallback');

  await go('networks');
  await page.reload();
  await selected(tab('ED2K'));
  await selected(tab('aMule log'));
  assert.deepEqual(errors, []);
  console.log('PASS: a page reload restores defaults; no browser runtime errors');
});
