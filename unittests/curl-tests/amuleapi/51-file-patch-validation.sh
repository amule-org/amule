#!/usr/bin/env bash
# Invalid later PATCH fields must leave the file unchanged. Uses an offline core/API pair.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" != "--fixture-ready" ]; then
	: "${AMULEAPI_BIN:?Set AMULEAPI_BIN}"
	. "$SCRIPT_DIR/lib/amuleapi-bin.sh"
	AMULED_BIN=${AMULED_BIN:-$(_find_amuled_bin "$AMULEAPI_BIN")}
	exec python3 "$SCRIPT_DIR/../../tests/AllSearchApiSmoke.py" \
		"$AMULED_BIN" "$AMULEAPI_BIN" "$SCRIPT_DIR/51-file-patch-validation.sh"
fi
python3 - "$API" "$SCRIPT_DIR/../../tests" <<'PY'
from pathlib import Path
import sys
import time

sys.path.insert(0, sys.argv[2])
from AllSearchIntegrationTest import http

base = sys.argv[1]
token = None
checks = 0


def call(method, path, body=None):
    return http(base, path, token=token, method=method, body=body)


def expect(method, path, body, status, code=None):
    global checks
    result = call(method, path, body)
    assert result[0] == status, (method, path, body, result)
    if code:
        assert result[1]['error']['code'] == code, result
    checks += 1
    return result[1]


def wait_for(read, label):
    for _ in range(100):
        value = read()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError('timed out waiting for ' + label)


def state(path):
    row = expect('GET', path, None, 200)
    keys = ('name', 'priority', 'priority_auto', 'my_comment', 'my_rating')
    if path.startswith('/downloads/'):
        keys += ('status', 'category_index', 'a4af_auto')
    return {key: row[key] for key in keys}


def rejected_without_change(path, body, status=400, code='bad_request'):
    before = state(path)
    result = expect('PATCH', path, body, status, code)
    # A successful PATCH of an independent fixture file runs RefresherTick after
    # the rejected request. It refreshes the changed rows without undoing anything
    # on the target, so a stale snapshot cannot hide an already-sent command.
    expect('PATCH', refresh_download, {'priority': 'low'}, 200)
    after = state(path)
    assert after == before, (body, before, after)
    return result


token = expect('POST', '/auth/login?include_token=true', {'password': 'adminpass'}, 200)['token']
expect('POST', '/categories', {'name': 'patch-validation'}, 202)
category = next(row['index'] for row in expect('GET', '/categories', None, 200)['categories']
                if row['name'] == 'patch-validation')
file_hash = 'deadc0de1234567890abcdef01234567'
link = f'ed2k://|file|patch-validation.bin|12345|{file_hash}|/'
refresh_hash = 'deadc0de1234567890abcdef01234568'
refresh_link = f'ed2k://|file|patch-refresh.bin|1024|{refresh_hash}|/'
refresh_download = '/downloads/' + refresh_hash
expect('POST', '/downloads', {'links': [link, refresh_link]}, 202)
wait_for(lambda: call('GET', refresh_download)[0] == 200, 'refresh fixture download')
download = '/downloads/' + file_hash
wait_for(lambda: call('GET', download)[0] == 200, 'download')
expect('PATCH', download, {'action': 'resume', 'priority': 'low', 'a4af_auto': False}, 200)

for invalid in ({'priority': []}, {'category_index': 2.9}, {'category_index': '2'},
                {'category_index': 256}, {'a4af_auto': 'false'}, {'my_comment': 'missing rating'},
                {'my_comment': 'x', 'my_rating': 2.9}, {'my_comment': 'x', 'my_rating': 1e100},
                {'name': ''}, {'name': '../outside'}):
    body = dict(action='pause', priority='high', category_index=category, a4af_auto=True)
    body.update(invalid)
    result = rejected_without_change(download, body)
    if 'category_index' in invalid:
        assert result['error']['message'] == '`category_index` must be an integer in 0..255'
print('PASS: invalid later download fields leave action, priority, category, A4AF and name unchanged')
for value in (2.9, '2', 256):
    result = expect('POST', '/search/results/' + file_hash + '/download',
                    {'category_index': value}, 400, 'bad_request')
    assert result['error']['message'] == '`category_index` must be an integer in 0..255'
print('PASS: search downloads use the same explicit integer-range error')
rejected_without_change(download, {'action': 'pause', 'my_comment': 'x', 'my_rating': 2},
                        409, 'not_shared')
valid = dict(action='pause', priority='high', category_index=category,
             a4af_auto=True, name='renamed-patch.bin')
expect('PATCH', download, valid, 200)
row = state(download)
assert row['status'] == 'paused' and row['priority'] == 'high', row
assert row['category_index'] == category and row['a4af_auto'] is True, row
assert row['name'] == 'renamed-patch.bin', row
print('PASS: a fully valid combined download patch applies every field')

# AllSearchApiSmoke owns HOME/core and its Incoming folder; never write to a user's shares.
incoming = Path.home() / 'core' / 'Incoming'
assert incoming.is_dir(), 'expected the owned offline fixture Incoming folder'
(incoming / 'shared-patch.bin').write_bytes(b'offline shared patch validation\n')
expect('POST', '/shared_reload', {}, 202)
shared_hash = wait_for(lambda: next((row['hash'] for row in
                                  expect('GET', '/shared', None, 200)['shared']
                                  if row['name'] == 'shared-patch.bin'), None), 'shared file')
shared = '/shared/' + shared_hash
for body in ({'priority': 'high', 'name': ''},
             {'priority': 'high', 'my_comment': 'missing rating'},
             {'priority': 'high', 'my_comment': 'x', 'my_rating': 1e100},
             {'my_comment': 'must not persist', 'my_rating': 4, 'name': '../outside'}):
    rejected_without_change(shared, body)
print('PASS: shared-file callers also validate before changing priority or comments')
expect('PATCH', shared, {'priority': 'high', 'my_comment': 'valid comment', 'my_rating': 4,
                         'name': 'renamed-shared.bin'}, 200)
row = state(shared)
assert row['priority'] == 'high' and row['name'] == 'renamed-shared.bin', row
assert row['my_comment'] == 'valid comment' and row['my_rating'] == 4, row
print('PASS: valid shared priority, comment, rating and rename commands still apply')
print(f'PASS: {checks} HTTP assertions; no network peers required')
PY
