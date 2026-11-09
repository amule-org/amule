#!/usr/bin/env python3
#
# This file is part of the aMule Project.
#
# Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
#
# Any parts of this program derived from the xMule, lMule or eMule project,
# or contributed by third-party developers are copyrighted by their
# respective authors.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
#
"""Status SSE must follow the real refresher through rejection, retries and recovery."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time

if shutil.which('curl') is None:
    print('SKIP: status event integration requires curl')
    sys.exit(77)

from AllSearchIntegrationTest import C
from MetricsIntegrationTest import Core, CoreHandler, api_fixture, eventually, request


class Events:
    def __init__(self, base, token):
        self.proc = subprocess.Popen([
            'curl', '--silent', '--show-error', '--no-buffer', '--noproxy', '*',
            '-H', 'Authorization: Bearer ' + token,
            '-H', 'Last-Event-ID: 0', base + '/api/v1/events'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.pending = b''
        self.seen = []

    def collect(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not select.select([self.proc.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                break
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            assert chunk, ('SSE closed unexpectedly', self.proc.poll())
            self.pending += chunk
            while b'\n\n' in self.pending:
                frame, self.pending = self.pending.split(b'\n\n', 1)
                fields = dict(line.split(b':', 1) for line in frame.splitlines() if b':' in line)
                if b'event' in fields and b'data' in fields:
                    self.seen.append((fields[b'event'].strip().decode(), fields[b'data'].strip().decode()))

    def wait(self, predicate):
        deadline = time.monotonic() + 6
        while not predicate(self.seen):
            assert time.monotonic() < deadline, self.seen
            self.collect(.1)

    def wait_closed(self):
        deadline = time.monotonic() + 6
        while select.select([self.proc.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
            if not os.read(self.proc.stdout.fileno(), 65536):
                # Exit 0: the server ended the chunked body, not just the connection.
                assert self.proc.wait(5) == 0, self.proc.stderr.read()
                return
        raise AssertionError('SSE stream still open')

    def close(self):
        self.proc.terminate()
        self.proc.communicate(timeout=5)


@contextmanager
def events(base, token):
    stream = Events(base, token)
    try:
        yield stream
    finally:
        stream.close()


def status_payloads(stream, cursor=0):
    return [data for name, data in stream.seen[cursor:] if name == 'status_changed']


def first_row(body, key):
    start = body.index('[', body.index('"' + key + '"')) + 1
    _, end = json.JSONDecoder().raw_decode(body, start)
    return body[start:end]


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-status-events-') as directory, \
            Core(('127.0.0.1', 0), CoreHandler) as core:
        core.ops, core.populated = [], True
        core.fail_opcode = C['EC_OP_STAT_REQ']
        core.hold, core.entered, core.release = threading.Event(), threading.Event(), threading.Event()
        threading.Thread(target=core.serve_forever, daemon=True).start()
        try:
            with api_fixture(binary, Path(directory) / 'api', core.server_address[1]) as (base, _):
                def login():
                    code, _, text = request(base, '/api/v1/auth/login?include_token=true', method='POST',
                                            body={'password': 'adminpass'})
                    assert code == 200, text
                    return json.loads(text)['token']
                token, stream_token = login(), login()
                with events(base, stream_token) as stream:
                    stream.collect(1.2)
                    assert not status_payloads(stream), stream.seen
                    assert request(base, '/api/v1/status', token)[0] == 503
                    core.fail_opcode = None
                    stream.wait(lambda _: any(json.loads(data)['ec_connected']
                                               for data in status_payloads(stream)))
                    assert status_payloads(stream)[-1] == request(base, '/api/v1/status', token)[2]
                    print('PASS: failed first load stays unavailable; first complete snapshot reaches SSE')

                    # Hold each failure for multiple ticks: one false event, no collection
                    # removals from ResetLists, then one true event on recovery.
                    for opcode in ('EC_OP_STAT_REQ', 'EC_OP_GET_UPDATE', 'EC_OP_GET_PREFERENCES'):
                        cursor = len(stream.seen)
                        core.fail_opcode = C[opcode]
                        stream.wait(lambda _: any(not json.loads(data)['ec_connected']
                                                   for data in status_payloads(stream, cursor)))
                        stream.collect(2.2)
                        failed = status_payloads(stream, cursor)
                        assert len(failed) == 1, (opcode, failed)
                        assert failed[0] == request(base, '/api/v1/status', token)[2]
                        assert not any(name.endswith(('_added', '_updated', '_removed'))
                                       for name, _ in stream.seen[cursor:]), stream.seen[cursor:]
                        core.fail_opcode = None
                        stream.wait(lambda _: any(json.loads(data)['ec_connected']
                                                   for data in status_payloads(stream, cursor)))
                        stream.collect(1.2)
                        recovered = status_payloads(stream, cursor)
                        assert len(recovered) == 2, (opcode, recovered)
                        assert recovered[-1] == request(base, '/api/v1/status', token)[2]
                    print('PASS: early/partial/late EC rejections notify once, preserve collection baselines and recover')

                    # Compare actual production HTTP list writers with the bus payload bytes.
                    for event, endpoint, key in (('client_added', '/clients', 'clients'),
                                                 ('server_added', '/servers', 'servers')):
                        payload = next(data for name, data in stream.seen if name == event)
                        code, _, body = request(base, '/api/v1' + endpoint, token)
                        assert code == 200, body
                        assert payload == first_row(body, key), (event, payload, first_row(body, key))
                    print('PASS: client/server events match the production REST list-item bytes')

                    # A late rejection can follow a changed STAT_REQ and empty collection
                    # replies. Publish each changed dashboard while still disconnected, but
                    # preserve the successful collection baseline through an unchanged recovery.
                    cursor = len(stream.seen)
                    core.fail_opcode = C['EC_OP_GET_PREFERENCES']
                    core.populated = False
                    stream.wait(lambda _: any(not json.loads(data)['ec_connected'] and
                                               json.loads(data)['ed2k']['state'] == 'disconnected'
                                               for data in status_payloads(stream, cursor)))
                    code, headers, body = request(base, '/api/v1/status', token)
                    assert code == 200 and body == status_payloads(stream, cursor)[-1]
                    assert request(base, '/api/v1/status', token,
                                   headers={'If-None-Match': headers['ETag']})[0] == 304
                    changed_cursor = len(stream.seen)
                    core.populated = True
                    stream.wait(lambda _: any(not json.loads(data)['ec_connected'] and
                                               json.loads(data)['ed2k']['state'] == 'connected'
                                               for data in status_payloads(stream, changed_cursor)))
                    changed = status_payloads(stream, changed_cursor)[-1]
                    code, _, body = request(base, '/api/v1/status', token,
                                             headers={'If-None-Match': headers['ETag']})
                    assert code == 200 and body == changed
                    stream.collect(1.2)
                    assert len(status_payloads(stream, changed_cursor)) == 1, stream.seen[changed_cursor:]
                    core.fail_opcode = None
                    stream.wait(lambda _: any(json.loads(data)['ec_connected']
                                               for data in status_payloads(stream, changed_cursor)))
                    stream.collect(1.2)
                    assert not any(name.endswith(('_added', '_updated', '_removed'))
                                   for name, _ in stream.seen[cursor:]), stream.seen[cursor:]
                    print('PASS: partial dashboards notify while failed; conditional REST and collection recovery agree')

                    code, headers, body = request(base, '/api/v1/status', token)
                    assert code == 200
                    code, _, empty = request(base, '/api/v1/status', token,
                                             headers={'If-None-Match': headers['ETag']})
                    assert code == 304 and not empty
                    code, head, empty = request(base, '/api/v1/status', token, method='HEAD')
                    assert code == 200 and not empty and head['ETag'] == headers['ETag']
                    assert int(head['Content-Length']) == len(body.encode())
                    print('PASS: status HEAD/ETag/304 remain consistent after recovery')

                    # A worker sees a closed client only when a write fails, and the first
                    # write after the client's FIN succeeds. Revoke the token instead, then
                    # empty the lists to wake the worker: it drops its subscription and ends
                    # the stream.
                    assert request(base, '/api/v1/auth/logout', stream_token, method='POST')[0] == 204
                    core.populated = False
                    stream.wait_closed()

                # The app suspends full diffs after five unsubscribed ticks. Resuming
                # primes through a scratch bus, then asks the subscriber to re-read.
                eventually(lambda: json.loads(request(base, '/api/v1/status', token)[2])['ed2k']['state']
                           == 'disconnected')
                ticks = core.ops.count(C['EC_OP_STAT_REQ'])
                eventually(lambda: core.ops.count(C['EC_OP_STAT_REQ']) >= ticks + 8, seconds=12)
                # Subscribe during a blackout. A failed tick must publish freshness without
                # consuming the pending idle resync or publishing empty collection deltas.
                core.fail_opcode = C['EC_OP_STAT_REQ']
                eventually(lambda: not json.loads(request(base, '/api/v1/status', token)[2])['ec_connected'])
                with events(base, token) as stream:
                    stream.wait(lambda _: any(not json.loads(data)['ec_connected']
                                               for data in status_payloads(stream)))
                    cursor = len(stream.seen)
                    stream.collect(1.2)
                    assert not stream.seen[cursor:], stream.seen[cursor:]
                    assert not any(name == 'resync' and json.loads(data)['reason'] == 'idle'
                                   for name, data in stream.seen), stream.seen
                    core.fail_opcode = None
                    stream.wait(lambda seen: any(name == 'resync' and json.loads(data)['reason'] == 'idle'
                                                 for name, data in seen))
                    assert json.loads(request(base, '/api/v1/status', token)[2])['ec_connected']
                    cursor = len(stream.seen)
                    stream.collect(1.2)
                    assert not stream.seen[cursor:], stream.seen[cursor:]
                    core.fail_opcode = C['EC_OP_STAT_REQ']
                    stream.wait(lambda _: any(not json.loads(data)['ec_connected']
                                               for data in status_payloads(stream, cursor)))
                    core.fail_opcode = None
                    stream.wait(lambda _: any(json.loads(data)['ec_connected']
                                               for data in status_payloads(stream, cursor)))
                print('PASS: failure preserves pending idle resync; recovery primes once and later failures still notify')
        finally:
            core.fail_opcode = None
            core.release.set()
            core.shutdown()


if __name__ == '__main__':
    run(sys.argv[1])
