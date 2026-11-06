// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared harness for the WebUI browser tests.
// Set AMULE_BROWSER_EXECUTABLE to use an existing Chromium installation.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');

const repoRoot = path.resolve(__dirname, '../../..');
const staticRoot = path.join(repoRoot, 'src/webapi/static');
const types = {
  '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.svg': 'image/svg+xml', '.png': 'image/png',
};
const cleanups = [];

const importMap = () => fs.readFileSync(path.join(staticRoot, 'index.html'), 'utf8')
  .match(/<script type="importmap">([\s\S]*?)<\/script>/)[1];

function sendFile(res, base, pathname) {
  const file = path.resolve(base, '.' + pathname);
  if (!file.startsWith(base + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  res.setHeader('Content-Type', types[path.extname(file)] || 'application/octet-stream');
  res.end(fs.readFileSync(file));
}

// Serves root on 127.0.0.1 with "/" as index.html. mounts maps a URL prefix
// such as '/flags/' to a directory. handler(req, res, url) runs first and
// returns true when it has answered.
async function serveStatic(root = staticRoot, { mounts = {}, handler } = {}) {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    if (handler && handler(req, res, url)) return;
    const pathname = url.pathname === '/' ? '/index.html' : url.pathname;
    const mount = Object.keys(mounts).find(prefix => pathname.startsWith(prefix));
    if (mount) sendFile(res, mounts[mount], pathname.slice(mount.length - 1));
    else sendFile(res, root, pathname);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  cleanups.push(() => server.close());
  server.origin = `http://127.0.0.1:${server.address().port}`;
  return server;
}

async function launchBrowser(extraArgs = []) {
  const browser = await chromium.launch({
    executablePath: process.env.AMULE_BROWSER_EXECUTABLE || undefined, args: extraArgs });
  cleanups.push(() => browser.close());
  return browser;
}

function collectPageErrors(page) {
  const errors = [];
  page.on('pageerror', error => { errors.push(error.message); console.error(error); });
  return errors;
}

// Runs main, closes every server and browser opened above, and fails the
// process on any error.
function run(main) {
  (async () => {
    try {
      await main();
    } finally {
      for (const cleanup of cleanups.reverse()) await cleanup();
    }
  })().catch(error => { console.error(error); process.exitCode = 1; });
}

module.exports = { repoRoot, staticRoot, importMap, serveStatic, launchBrowser, collectPageErrors, run };
