#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise ordered addresses.dat failover through local HTTP, EC and amuled."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.error import URLError, HTTPError
from urllib.request import ProxyHandler, Request, build_opener

from AllSearchIntegrationTest import C, free_ports, stop_daemon, string, retry_port_collisions
from DaemonConfig import EC_PASSWORD, write_amule_conf
from KnownHistoryIntegrationTest import start

SERVER_MET = b'\xe0' + struct.pack('<I', 1) + bytes([203, 0, 113, 7]) + struct.pack('<HI', 4661, 0)
EXISTING = SERVER_MET.replace(bytes([203, 0, 113, 7]), bytes([203, 0, 113, 9]))
TRUNCATED = b'\xe0' + struct.pack('<I', 2) + bytes([203, 0, 113, 8]) + struct.pack('<HI', 4661, 0)
GOOD = (200, SERVER_MET)
FAIL = (503, b'unavailable')


def wait_for(proc, predicate, root):
    deadline = time.monotonic() + 20
    while not predicate():
        if proc.poll() is not None or time.monotonic() > deadline:
            raise AssertionError((root / 'logfile').read_text(errors='replace'))
        time.sleep(0.05)


def api_post(endpoint, path, payload, token=None):
    headers = {'Content-Type': 'application/json', 'Accept': 'application/jwt'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    req = Request(endpoint + path, json.dumps(payload).encode(), headers)
    with build_opener(ProxyHandler({})).open(req, timeout=2) as response:
        return response.status, response.read()


def start_api(binary, root, ec_port):
    root.mkdir()
    port = free_ports(1)[0]
    config = root / 'amuleapi.conf'
    config.write_text(f'[Server]\nBindAddress=127.0.0.1\nPort={port}\n'
                      f'[EC]\nHost=127.0.0.1\nPort={ec_port}\nPassword={EC_PASSWORD}\n')
    config.chmod(0o600)
    env = dict(os.environ, HOME=str(root), LC_ALL='C.UTF-8', LANG='C.UTF-8')
    with (root / 'console.log').open('w') as output:
        subprocess.run([binary, '--config-dir=' + str(root), '--set-admin-pass=fallback-test'],
                       env=env, stdout=output, stderr=subprocess.STDOUT, timeout=15, check=True)
        proc = subprocess.Popen([binary, '--config-dir=' + str(root), '--foreground', '--no-log-file'],
                                env=env, stdout=output, stderr=subprocess.STDOUT)
    endpoint = f'http://127.0.0.1:{port}/api/v1'
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                status, data = api_post(endpoint, '/auth/login', {'password': 'fallback-test'})
                assert status == 200, data
                return proc, endpoint, json.loads(data)['token']
            except (URLError, HTTPError):
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError((root / 'console.log').read_text(errors='replace'))
                time.sleep(0.05)
    except BaseException:
        proc.terminate()
        proc.wait(timeout=10)
        raise


def run(binary, root, responses, sources, expected, *, url='/primary', startup=False,
        success=True, reload=False, overlap=False, skipped=False, cancel=False, automatic=False, api_binary=None):
    requests = []
    release = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            if (overlap or cancel) and self.path == url:
                release.wait(timeout=20)
            status, body = responses.get(self.path, FAIL)
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                assert cancel

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    proc = ec = api_proc = None
    try:
        root.mkdir()
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        if not startup:
            (root / 'server.met').write_bytes(EXISTING)
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        base = f'http://127.0.0.1:{server.server_port}'
        source_file = root / 'addresses.dat'

        def source_text(paths):
            return ''.join((base + entry if entry.startswith('/') else entry) + '\n' for entry in paths)

        # An absent addresses.dat must remain absent; no shipped source is installed.
        if sources is not None and not reload:
            source_file.write_text(source_text(sources))
        requested = base + url if url.startswith('/') else url
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToED2K=1,
                         Ed2kServersUrl=requested if startup else base + '/primary',
                         Serverlist=int(automatic))
        proc, ec = start(binary, root, ports)
        if reload:
            source_file.write_text(source_text(sources))
        expected_source_file = source_file.read_bytes() if source_file.exists() else None
        offset = 0 if startup else len((root / 'logfile').read_text(errors='replace'))
        if api_binary:
            api_proc, endpoint, token = start_api(api_binary, root / 'api', ports[0])
            status, data = api_post(endpoint, '/servers_update', {'url': requested}, token)
            assert status == 202, (status, data)
        elif not startup and not automatic:
            op, _ = ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'],
                            [string(C['EC_TAG_SERVERS_UPDATE_URL'], requested)])
            assert op == C['EC_OP_NOOP'], op
        if overlap:
            wait_for(proc, lambda: url in requests, root)
            # An active sequence retains its original sources. A second request must
            # neither start another download nor replace the active fallback list.
            source_file.write_text(source_text(['/changed']))
            expected_source_file = source_file.read_bytes()
            op, _ = ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'],
                            [string(C['EC_TAG_SERVERS_UPDATE_URL'], base + '/overlap')])
            assert op == C['EC_OP_NOOP'], op
            wait_for(proc, lambda: 'A server list download is already in progress.' in
                     (root / 'logfile').read_text(errors='replace')[offset:], root)
            release.set()

        if cancel:
            wait_for(proc, lambda: url in requests, root)
            stop_daemon(proc)
            release.set()

        marker = ('Finished downloading the server list' if success else
                  'Skipped download of server.met' if skipped else
                  f'Failed to download server.met from {base + expected[-1]}' if expected else 'Invalid URL')
        if not cancel:
            wait_for(proc, lambda: marker in (root / 'logfile').read_text(errors='replace')[offset:], root)
        # Allow erroneous retries to reach the server before inspecting the sequence.
        time.sleep(0.3)
        assert requests == expected, requests
        data = (root / 'server.met').read_bytes() if (root / 'server.met').exists() else b''
        if not startup or success:
            assert int.from_bytes(data[1:5], 'little') == (1 if startup or not success else 2), data
        assert bytes([203, 0, 113, 8]) not in data, data
        if not success and not startup:
            assert bytes([203, 0, 113, 9]) in data, data
        assert (source_file.read_bytes() if source_file.exists() else None) == expected_source_file
    finally:
        release.set()
        if api_proc:
            api_proc.terminate()
            api_proc.wait(timeout=10)
        if ec:
            ec.sock.close()
        if proc and proc.poll() is None:
            stop_daemon(proc)
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)

    if (root / 'server.met').exists():
        assert bytes([203, 0, 113, 8]) not in (root / 'server.met').read_bytes()


def main():
    binary = str(Path(sys.argv[1]).resolve())
    api_binary = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-server-fallback-') as temp:
            root = Path(temp)

            def case(name, responses, sources, expected, **kwargs):
                run(binary, root / name, responses, sources, expected, **kwargs)

            if api_binary:
                case('api-primary-success', {'/primary': GOOD}, ['/backup'], ['/primary'], api_binary=api_binary)
                case('api-explicit-fallback', {'/backup': GOOD}, ['/backup'], ['/alternate', '/backup'],
                     url='/alternate', api_binary=api_binary)
                case('api-ordered-retries', {'/last': GOOD}, ['/first', '/last'],
                     ['/primary', '/first', '/last'], api_binary=api_binary)
                return

            case('primary-success', {'/primary': GOOD}, ['/backup'], ['/primary'])
            case('first-run', {'/backup': GOOD}, ['/backup'], ['/primary', '/backup'], startup=True)
            case('http-failure', {'/backup': GOOD}, ['/backup'], ['/primary', '/backup'])
            case('invalid-primary', {'/primary': (200, b'invalid'), '/backup': GOOD},
                 ['/backup'], ['/primary', '/backup'])
            case('truncated-primary', {'/primary': (200, TRUNCATED), '/backup': GOOD},
                 ['/backup'], ['/primary', '/backup'])
            case('ordered-retries', {'/first': (200, b'invalid'), '/last': GOOD},
                 ['/first', '/second', '/last', '/unused'], ['/primary', '/first', '/second', '/last'])
            case('all-fail', {}, ['/first', '/last'], ['/primary', '/first', '/last'], success=False)
            case('truncated-backup', {'/backup': (200, TRUNCATED)}, ['/backup'],
                 ['/primary', '/backup'], success=False)
            case('invalid-backup', {'/backup': (200, b'invalid')}, ['/backup'],
                 ['/primary', '/backup'], success=False)
            case('missing-sources', {}, None, ['/primary'], success=False)
            case('empty-sources', {}, [], ['/primary'], success=False)
            case('explicit-url', {'/backup': GOOD}, ['/backup'], ['/alternate', '/backup'], url='/alternate')
            case('deduplicated', {'/backup': GOOD},
                 ['/primary', '/primary', '# comment', 'ftp://unsupported/list', '/backup', '/backup'],
                 ['/primary', '/backup'])
            case('exhausted-duplicates', {}, ['/primary', '/backup', '/backup'],
                 ['/primary', '/backup'], success=False)
            case('live-edit', {'/backup': GOOD}, ['/backup'], ['/primary', '/backup'], reload=True)
            case('overlap-snapshot', {'/backup': GOOD}, ['/backup'],
                 ['/primary', '/backup'], overlap=True)
            case('not-modified', {'/primary': (304, b'')}, ['/backup'], ['/primary'],
                 success=False, skipped=True)
            case('invalid-requested-url', {'/backup': GOOD}, ['/backup'], ['/backup'], url='invalid')
            case('invalid-startup-url', {'/backup': GOOD}, ['/backup'], ['/backup'],
                 url='invalid', startup=True)
            case('invalid-url-no-sources', {}, [], [], url='invalid', success=False)
            case('invalid-sources', {}, ['# comment', 'ftp://unsupported/list', 'invalid'],
                 ['/primary'], success=False)
            case('cancel-manual-on-shutdown', {}, ['/backup'], ['/primary'], success=False, cancel=True)
            case('cancel-auto-on-shutdown', {}, ['/primary', '/backup'], ['/primary'],
                 success=False, cancel=True, automatic=True)
    retry_port_collisions(attempt)
    print('3 API fallback scenarios passed' if api_binary else '23 server-list fallback scenarios passed')


if __name__ == '__main__':
    main()
