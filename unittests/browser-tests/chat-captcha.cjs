// Run with Node and Playwright installed: node unittests/browser-tests/chat-captcha.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');
const staticRoot = path.resolve(__dirname, '../../src/webapi/static');
const importMap = fs.readFileSync(path.join(staticRoot, 'index.html'), 'utf8')
  .match(/<script type="importmap">([\s\S]*?)<\/script>/)[1];
const html = `<!doctype html><html><head><link rel="stylesheet" href="/css/app.css">
<script type="importmap">${importMap}</script></head><body><div id="app" style="height:600px"></div>
<script type="module">
import Messages from '/js/views/messages.js';
import { html, render } from '/js/dom.js';
import { store } from '/js/store.js';
import { chats } from '/js/chats.js';
window.store = store; window.chats = chats;
location.hash = 'messages';
render(html\`<\${Messages} isGuest=\${false} />\`, document.querySelector('#app'));
window.ready = true;
</script></body></html>`;
const bmp = Buffer.alloc(62 + 12 * 36);
bmp.write('BM'); bmp.writeUInt32LE(bmp.length, 2); bmp.writeUInt32LE(62, 10);
bmp.writeUInt32LE(40, 14); bmp.writeInt32LE(92, 18); bmp.writeInt32LE(36, 22);
bmp.writeUInt16LE(1, 26); bmp.writeUInt16LE(1, 28); bmp.fill(255, 58, 61);
const image = bmp.toString('base64');
let pending = 42, requests = [], reject = false, hold = false, closed = false, respond;
const row = () => ({ address: '127.0.0.1:4662', ip: '127.0.0.1', port: 4662, name: 'peer',
  client_ecid: 1, last_message_id: 42, pending_captcha_id: pending || null,
  pending_captcha_image: pending ? image : null });
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const json = (value, status = 200) => { res.writeHead(status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(value)); };
  if (url.pathname === '/') { res.setHeader('Content-Type', 'text/html'); res.end(html); return; }
  if (url.pathname.startsWith('/api/v1/')) {
    if (req.method === 'DELETE') { closed = true; json({}); return; }
    if (req.method === 'POST') {
      let body = ''; req.on('data', chunk => { body += chunk; });
      req.on('end', () => {
        requests.push(JSON.parse(body));
        const complete = () => reject
          ? json({ error: { code: 'captcha_expired', message: 'CAPTCHA challenge is no longer pending' } }, 409)
          : json({ address: row().address, message: { id: 60 + requests.length, text: JSON.parse(body).text } }, 202);
        if (hold) respond = complete; else complete();
      }); return;
    }
    if (url.pathname === '/api/v1/chats') json({ chats: closed ? [] : [row()] });
    else if (url.pathname.includes('/messages')) json({ address: row().address, messages: [] });
    else if (url.pathname.endsWith('/friends')) json({ friends: [] });
    else json({});
    return;
  }
  const file = path.resolve(staticRoot, '.' + url.pathname);
  if (!file.startsWith(staticRoot + path.sep) || !fs.existsSync(file)) { res.writeHead(404); res.end(); return; }
  const types = { '.js': 'text/javascript', '.json': 'application/json', '.css': 'text/css' };
  res.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
  res.end(fs.readFileSync(file));
});
(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ executablePath: process.env.AMULE_BROWSER_EXECUTABLE || undefined, args: ['--no-zygote', '--disable-gpu'] });
  try {
    const page = await browser.newPage();
    const errors = []; page.on('pageerror', error => { errors.push(error.message); console.error(error); });
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => window.ready);
    await page.waitForTimeout(500);
    await page.waitForFunction(() => document.querySelector('.chat-captcha')?.naturalWidth > 0);
    // The pending image is shown even though it has been evicted from history.
    assert.ok(await page.locator('.chat-text').textContent().then(text => text.includes('Solve')));
    const input = page.locator('input[name="message"]');
    await input.fill('late answer');
    pending = 0; reject = true;
    await page.evaluate(() => window.store.set('chat:updated', {
      address: '127.0.0.1:4662', pending_captcha_id: null
    }));
    await page.waitForFunction(() => window.store.get('chats').tabs[0].pendingCaptchaId === 0);
    // Restore a historical row so expiry is visibly distinguished from pending.
    await page.evaluate(image => window.store.set('chat:127.0.0.1:4662', [
      { id: 42, direction: 'in', type: 'captcha_challenge', captcha_image: image }
    ]), image);
    await page.waitForFunction(() => document.querySelector('.chat-text')?.textContent.includes('no longer pending'));
    await input.press('Enter');
    await page.waitForFunction(() => document.querySelector('.toast')?.textContent.includes('no longer pending'));
    assert.deepEqual(requests[0], { text: 'late answer', captcha_id: 42 });
    assert.equal(await input.inputValue(), 'late answer'); // failure preserves the draft
    assert.ok(await page.locator('.toast').textContent().then(text => text.includes('Send the text again')));
    // Retrying the retained draft must stop sending the retired challenge id.
    reject = false;
    await input.press('Enter');
    await page.waitForFunction(() => document.querySelector('input[name="message"]').value === '');
    assert.deepEqual(requests.at(-1), { text: 'late answer' });
    // A replacement learned only from the failure refresh becomes the retry id.
    pending = 43;
    await page.evaluate(async () => { await window.chats.adopt(); });
    await input.fill('replacement answer');
    pending = 44; reject = true;
    await input.press('Enter');
    await page.waitForFunction(() => document.querySelector('.toast')?.textContent.includes('Use the latest challenge'));
    assert.deepEqual(requests.at(-1), { text: 'replacement answer', captcha_id: 43 });
    assert.equal(await input.inputValue(), 'replacement answer');
    reject = false;
    await input.press('Enter');
    await page.waitForFunction(() => document.querySelector('input[name="message"]').value === '');
    assert.deepEqual(requests.at(-1), { text: 'replacement answer', captcha_id: 44 });
    pending = 0;
    await page.evaluate(async () => { await window.chats.adopt(); });
    await input.fill(''); await input.fill('ordinary message');
    reject = false;
    await input.press('Enter');
    await page.waitForFunction(() => document.querySelector('input[name="message"]').value === '');
    assert.deepEqual(requests.at(-1), { text: 'ordinary message' });
    // Sending a live answer includes the id; a response cannot erase a newer draft.
    pending = 43; hold = true;
    await page.evaluate(async () => { await window.chats.adopt(); });
    await input.fill('current answer'); await input.press('Enter');
    await page.waitForTimeout(100);
    assert.deepEqual(requests.at(-1), { text: 'current answer', captcha_id: 43 });
    await input.fill('new draft'); respond();
    await page.waitForTimeout(100);
    assert.equal(await input.inputValue(), 'new draft');
    // A rejected older send must not rebind text edited during the request.
    pending = 45; reject = true;
    await page.evaluate(async () => { await window.chats.adopt(); });
    await input.fill(''); await input.fill('old draft'); await input.press('Enter');
    await page.waitForTimeout(100);
    pending = 46;
    await page.evaluate(async () => { await window.chats.adopt(); });
    await input.fill(''); await input.fill('new answer');
    pending = 47; respond();
    await page.waitForFunction(() => window.store.get('chats').tabs[0].pendingCaptchaId === 47);
    await page.waitForTimeout(100);
    assert.equal(await input.inputValue(), 'new answer');
    reject = false; hold = false;
    await input.press('Enter');
    await page.waitForFunction(() => document.querySelector('input[name="message"]').value === '');
    assert.deepEqual(requests.at(-1), { text: 'new answer', captcha_id: 46 });
    // Closing during a send must not resurrect the tab when the reply arrives.
    hold = true;
    await input.fill(''); await input.fill('closing'); await input.press('Enter');
    await page.waitForTimeout(100);
    await page.evaluate(async () => { await window.chats.close('127.0.0.1:4662'); });
    respond();
    await page.waitForTimeout(100);
    assert.equal(await page.evaluate(() => window.store.get('chats').tabs.length), 0);
    // A failure's refresh must also respect a tab closed during the request.
    closed = false; pending = 48; reject = true;
    await page.evaluate(async () => { await window.chats.adopt(); });
    await input.fill('closed answer'); await input.press('Enter');
    await page.waitForTimeout(100);
    assert.deepEqual(requests.at(-1), { text: 'closed answer', captcha_id: 48 });
    await page.evaluate(async () => { await window.chats.close('127.0.0.1:4662'); });
    respond();
    await page.waitForFunction(() => document.querySelector('.toast')?.textContent.includes('Send the text again'));
    assert.equal(await page.evaluate(() => window.store.get('chats').tabs.length), 0);
    assert.equal(await input.inputValue(), '');
    assert.deepEqual(errors, []);
    console.log('PASS: pending image recovery, expiry, retained-draft retries with no/replacement challenge, ordinary messages, concurrent edits after successful/rejected sends, and tab closure');
  } finally { await browser.close(); server.close(); }
})().catch(error => { console.error(error); server.close(); process.exitCode = 1; });
