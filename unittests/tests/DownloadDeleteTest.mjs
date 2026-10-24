// Run the production confirmation/selection helpers without a browser or a daemon.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
// These small, immutable fixtures need no asynchronous filesystem scheduling.
const source = readFileSync(new URL('../../src/webapi/static/js/download-delete.js', import.meta.url), 'utf8');
const { prepareDownloadDeletion, prepareSharedDeletion, selectionAfterDeletion } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));

const selection = ['a', 'b'];
let release;
const pause = new Promise((resolve) => { release = resolve; });
const pending = prepareDownloadDeletion(selection, async (hash) => {
  await pause;
  return { ecid: hash === 'a' ? 1 : 2, status: 'completed', name: hash, directory: '/Incoming' };
});
selection.push('unconfirmed');
release();
const prepared = await pending;
assert.deepEqual(prepared.hashes, ['a', 'b']);
assert.deepEqual(Object.keys(prepared.confirmed_target), ['a', 'b']);
assert.deepEqual(prepared.confirmed_target.a, { ecid: 1, status: 'completed', name: 'a', directory: '/Incoming' });
assert.deepEqual([...selectionAfterDeletion(new Set(['a', 'b', 'later']), prepared.hashes,
  [{ id: 'a', ok: true }, { id: 'b', ok: false }])], ['b', 'later']);
assert.deepEqual([...selectionAfterDeletion(new Set(['a', 'later']), ['a'])], ['later']);
// A changed/removed file fails preparation: no incomplete set can be confirmed.
await assert.rejects(prepareDownloadDeletion(['a', 'missing'], async (h) => {
  if (h === 'missing') throw new Error('gone');
  return { ecid: 1, status: 'completed', name: h, directory: '/Incoming' };
}), /gone/);
// Every file, including those beyond the ten displayed paths, has a precondition.
const many = await prepareDownloadDeletion(Array.from({ length: 25 }, (_, i) => '' + i),
  async (h) => ({ ecid: +h + 1, status: 'completed', name: h, directory: '/Incoming' }));
assert.equal(Object.keys(many.confirmed_target).length, 25);
console.log('PASS: confirmed selection frozen; failed/new selections retained; all targets guarded');

// All-503 bulk errors use a results envelope, rather than a top-level error.
// Preserve the actionable per-file error instead of reducing it to "HTTP 503".
const apiSource = readFileSync(new URL('../../src/webapi/static/js/api.js', import.meta.url), 'utf8')
  .replace('import { t } from "./i18n.js";', 'const t = (key) => key;');
globalThis.window = { location: { pathname: "/" } };
const { api } = await import('data:text/javascript;base64,' + Buffer.from(apiSource).toString('base64'));
globalThis.fetch = async () => ({ status: 503, ok: false,
  text: async () => JSON.stringify({ results: [{ id: 'a', ok: false,
    error: { code: 'ec_unsupported', message: 'Core does not support deletion' } }] }) });
await assert.rejects(api.del('downloads', { hashes: ['a'] }),
  (error) => error.code === 'ec_unsupported' && error.message === 'Core does not support deletion');
console.log('PASS: actionable bulk compatibility errors preserved');

const sharedFile = { ecid: 8, name: 'shared.txt', directory: '/Shared', incomplete: false };
const frozen = await prepareSharedDeletion(['shared'], async () => sharedFile);
sharedFile.name = 'replacement.txt';
sharedFile.ecid = 9;
assert.deepEqual(frozen.confirmed_target.shared,
  { ecid: 8, name: 'shared.txt', directory: '/Shared', incomplete: false });
console.log('PASS: shared confirmation freezes directory, name, record id, and incomplete state');

// Exercise the production shared action across the asynchronous dialog boundary.
const calls = [];
let accept = false;
let incomplete = false;
globalThis.sharedDeleteDeps = {
  api: {
    get: async () => ({ ecid: 12, name: 'confirmed.txt', directory: '/Library', incomplete }),
    del: async (...args) => calls.push(['DELETE', ...args]),
  },
  data: { refresh: (key) => calls.push(['refresh', key]) },
  confirmDialog: async (question) => { calls.push(['confirm', question]); return accept; },
  toast: (...args) => calls.push(['toast', ...args]),
  t: (key, params) => params ? params.name + '\n' + params.directory : key,
  terr: (e) => e.message,
  prepareSharedDeletion,
};
let actionSource = readFileSync(new URL('../../src/webapi/static/js/shared-delete.js', import.meta.url), 'utf8');
actionSource = actionSource.replace(/^import .*;$/gm, '');
actionSource = 'const { api, data, confirmDialog, toast, t, terr, prepareSharedDeletion } = globalThis.sharedDeleteDeps;\n' + actionSource;
const { deleteSharedFile } = await import('data:text/javascript;base64,' + Buffer.from(actionSource).toString('base64'));
await deleteSharedFile('shared');
assert.deepEqual(calls, [['confirm', 'confirmed.txt\n/Library']]);
calls.length = 0;
accept = true;
await deleteSharedFile('shared');
assert.deepEqual(calls, [
  ['confirm', 'confirmed.txt\n/Library'],
  ['DELETE', 'shared/shared', { confirmed_target: { ecid: 12, name: 'confirmed.txt', directory: '/Library', incomplete: false } }],
  ['refresh', 'shared'], ['refresh', 'downloads'],
]);
calls.length = 0;
incomplete = true;
await deleteSharedFile('shared');
assert.deepEqual(calls, [['toast', 'shared_delete_incomplete', 'error']]);
console.log('PASS: shared action names its file and directory, respects cancellation, requires confirmation, and refreshes both views');
