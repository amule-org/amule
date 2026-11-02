#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise shared addresses.dat updates through local HTTP, EC and amuled."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.error import URLError, HTTPError
from urllib.request import ProxyHandler, Request, build_opener

from AllSearchIntegrationTest import (C, configure_amuleapi, exact, free_ports, integer,
                                      retry_port_collisions, running_daemon, start_daemon,
                                      stop_daemon, string, tag)
from DaemonConfig import daemon_env, write_amule_conf

SERVER_MET = b'\xe0' + struct.pack('<I', 1) + bytes([203, 0, 113, 7]) + struct.pack('<HI', 4661, 0)
EXISTING = SERVER_MET.replace(bytes([203, 0, 113, 7]), bytes([203, 0, 113, 9]))
TRAILING = SERVER_MET.replace(bytes([203, 0, 113, 7]), bytes([203, 0, 113, 8])) + b'garbage'
TRUNCATED = b'\xe0' + struct.pack('<I', 2) + bytes([203, 0, 113, 8]) + struct.pack('<HI', 4661, 0)
GOOD = (200, SERVER_MET)
SECOND = (200, SERVER_MET.replace(bytes([203, 0, 113, 7]), bytes([203, 0, 113, 10])))
FAIL = (503, b'unavailable')


def start(binary, root, ports, *, ed2k=True):
    return start_daemon(binary, root, ports, server_udp=ed2k)


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
                      f'[EC]\nHost=127.0.0.1\nPort={ec_port}\n')
    config.chmod(0o600)
    env = daemon_env(root, LANG='C.UTF-8')
    with (root / 'console.log').open('w') as output:
        configure_amuleapi(binary, root, env, output, admin='fallback-test')
        proc = subprocess.Popen([binary, '--config-dir=' + str(root), '--foreground', '--no-log-file'],
                                env=env, stdout=output, stderr=subprocess.STDOUT)
    endpoint = f'http://127.0.0.1:{port}/api/v1'
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                status, data = api_post(endpoint, '/auth/login', {'password': 'fallback-test'})
                assert status == 200, data
                token = json.loads(data)['token']
                ready = Request(endpoint + '/preferences', headers={'Authorization': 'Bearer ' + token})
                with build_opener(ProxyHandler({})).open(ready, timeout=2) as response:
                    assert response.status == 200
                    response.read()
                return proc, endpoint, token
            except (URLError, HTTPError):
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError((root / 'console.log').read_text(errors='replace'))
                time.sleep(0.05)
    except BaseException:
        stop_daemon(proc, check=False)
        raise


def run(binary, root, responses, sources, expected, *, url=None, startup=False,
        success=True, reload=False, overlap=False, skipped=False, cancel=False,
        automatic=False, api_binary=None, legacy='', missing=False, connects=False):
    requests = []
    release = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            if (overlap or cancel) and self.path == expected[0]:
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

        if sources is not None and not reload:
            source_file.write_text(source_text(sources))
        requested = base + url if url and url.startswith('/') else url
        legacy_url = base + legacy if legacy.startswith('/') else legacy
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToED2K=1,
                         Ed2kServersUrl=legacy_url, Serverlist=int(automatic))
        proc, ec = start(binary, root, ports)
        if reload:
            source_file.write_text(source_text(sources))
        if missing:
            source_file.unlink()
        expected_source_file = source_file.read_bytes() if source_file.exists() else None
        if api_binary:
            api_proc, endpoint, token = start_api(api_binary, root / 'api', ports[0])
            status, data = api_post(endpoint, '/servers_update', {} if requested is None else {'url': requested}, token)
            assert status == 202, (status, data)
        elif not startup and not automatic:
            tags = [] if requested is None else [string(C['EC_TAG_SERVERS_UPDATE_URL'], requested)]
            op, result = ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'], tags)
            assert op == C['EC_OP_NOOP' if expected else 'EC_OP_FAILED'], (op, result)
        if not expected:
            reason = 'Invalid URL' if requested else 'No server list sources configured'
            wait_for(proc, lambda: reason in (root / 'logfile').read_text(errors='replace'), root)
        if overlap:
            wait_for(proc, lambda: len(requests) == 1, root)
            # The current sequence must retain its snapshot despite live edits and another request.
            source_file.write_text(source_text(['/changed']))
            expected_source_file = source_file.read_bytes()
            op, refusal = ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'],
                            [string(C['EC_TAG_SERVERS_UPDATE_URL'], base + '/overlap')])
            assert op == C['EC_OP_FAILED'], op
            assert refusal[C['EC_TAG_SERVER_UPDATE_BUSY']][0] == 1, refusal
            if api_binary:
                try:
                    api_post(endpoint, '/servers_update', {}, token)
                    raise AssertionError('overlapping API update was accepted')
                except HTTPError as error:
                    assert error.code == 409, error.code
                    assert 'already in progress' in error.read().decode()
            wait_for(proc, lambda: 'A server list download is already in progress.' in
                     (root / 'logfile').read_text(errors='replace'), root)
            release.set()
        if cancel:
            wait_for(proc, lambda: len(requests) == 1, root)
            stop_daemon(proc)
            release.set()
        else:
            wait_for(proc, lambda: len(requests) >= len(expected), root)
            if expected:
                # URL diagnostics intentionally reveal only the origin, never its path/query.
                def completed():
                    log = (root / 'logfile').read_text(errors='replace')
                    return (log.count('Finished downloading the server list from ' + base) +
                            log.count('Failed to download server.met from ' + base) +
                            log.count('Skipped download of server.met')) >= len(expected)
                wait_for(proc, completed, root)
            if connects:
                # SERVER_MET's only entry; nothing listens there, so the attempt is all we see.
                wait_for(proc, lambda: 'Connecting to Server 203.0.113.7' in
                         (root / 'logfile').read_text(errors='replace'), root)
            time.sleep(0.3)
        if cancel:
            assert not (root / 'server.met.download').exists()
        assert requests == expected, requests
        data = (root / 'server.met').read_bytes() if (root / 'server.met').exists() else b''
        assert bytes([203, 0, 113, 8]) not in data, data
        if not startup:
            assert bytes([203, 0, 113, 9]) in data, data
        if success:
            assert bytes([203, 0, 113, 7]) in data, data
            expected_ips = {9} if not startup else set()
            for path in expected:
                status, body = responses.get(path, FAIL)
                if status == 200 and body in (GOOD[1], SECOND[1]):
                    expected_ips.add(body[8])
            assert int.from_bytes(data[1:5], 'little') == len(expected_ips), (expected_ips, data)
            for last in expected_ips:
                assert bytes([203, 0, 113, last]) in data, data
        assert (source_file.read_bytes() if source_file.exists() else None) == expected_source_file
        # A one-off request must never reintroduce the retired config key.
        assert 'Ed2kServersUrl=' not in (root / 'amule.conf').read_text()
    finally:
        release.set()
        if api_proc:
            # amuleapi exits 1 once it has seen its EC connection drop, which stopping the
            # core above can cause.
            stop_daemon(api_proc, check=sys.exc_info()[0] is None, codes=(0, 1))
        if ec:
            ec.sock.close()
        if proc and proc.poll() is None:
            stop_daemon(proc)
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def connection_case(binary, root, already_connecting):
    """A multi-source update connects once at completion and preserves an active login."""
    root.mkdir()
    (root / 'Incoming').mkdir()
    (root / 'Temp').mkdir()
    empty_met = b'\xe0' + struct.pack('<I', 0)
    (root / 'server.met').write_bytes(empty_met)
    source_file = root / 'addresses.dat'
    source_file.write_text('')
    last_requested = threading.Event()
    release = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == '/last':
                last_requested.set()
                release.wait(timeout=20)
            status, body = (200, empty_met) if self.path == '/first' else FAIL
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    proc = ec = peer = None
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(4)
        listener.settimeout(0.5)

        def no_new_connection():
            try:
                unexpected, _ = listener.accept()
            except socket.timeout:
                return
            unexpected.close()
            raise AssertionError('server update started or restarted an ED2K connection')

        def accept_login():
            listener.settimeout(5)
            accepted, _ = listener.accept()
            listener.settimeout(0.5)
            accepted.settimeout(5)
            try:
                _, length = struct.unpack('<BI', exact(accepted, 5))
                assert exact(accepted, length)[0] == 0x01  # OP_LOGINREQUEST
                # Withhold the ID reply, keeping the daemon in IsConnecting().
                return accepted
            except BaseException:
                accepted.close()
                raise

        try:
            ports = free_ports(3, udp_indices=(2,), peer_index=1)
            write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2],
                             ConnectToED2K=1, FilterLanIPs=0)
            proc, ec = start(binary, root, ports)
            address = f'localhost:{listener.getsockname()[1]}'
            assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], address)])[0] == C['EC_OP_NOOP']
            if already_connecting:
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                peer = accept_login()
            base = f'http://127.0.0.1:{server.server_port}'
            source_file.write_text(base + '/first\n' + base + '/last\n')
            assert ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'])[0] == C['EC_OP_NOOP']
            wait_for(proc, last_requested.is_set, root)
            # The first valid source has loaded, but the sequence is still running.
            _, state = ec.call(C['EC_OP_GET_CONNSTATE'])
            assert state[C['EC_TAG_CONNSTATE']][0] & 0x03 == (0x02 if already_connecting else 0), state
            no_new_connection()
            release.set()
            wait_for(proc, lambda: 'Failed to download server.met from ' + base in
                     (root / 'logfile').read_text(errors='replace'), root)
            if not already_connecting:
                # The last source failed: success must remember the earlier source.
                peer = accept_login()
            no_new_connection()
            peer.settimeout(0.5)
            try:
                assert peer.recv(1), 'the pending ED2K login was disconnected'
            except socket.timeout:
                pass  # The pending login is still open, with no reply from our server.

            # A subsequent wholly failed update must not reuse the previous success.
            assert ec.call(C['EC_OP_SERVER_DISCONNECT'])[0] == C['EC_OP_NOOP']
            peer.close()
            peer = None
            source_file.write_text(base + '/failure\n')
            failures = (root / 'logfile').read_text(errors='replace').count('Failed to download server.met from ')
            assert ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'])[0] == C['EC_OP_NOOP']
            wait_for(proc, lambda: (root / 'logfile').read_text(errors='replace').count(
                     'Failed to download server.met from ') > failures, root)
            no_new_connection()
        finally:
            release.set()
            if peer:
                peer.close()
            if ec:
                ec.sock.close()
            if proc and proc.poll() is None:
                stop_daemon(proc)
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


def editor_case(binary, api_binary, root):
    root.mkdir()
    (root / 'Incoming').mkdir()
    (root / 'Temp').mkdir()
    (root / 'server.met').write_bytes(EXISTING)
    source_file = root / 'addresses.dat'
    original = '# Preserve comments when the legacy source is already present.\nhttps://upd.emule-security.org/server.met\nhttps://one.example/list\n'
    source_file.write_text(original)
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2],
                     Ed2kServersUrl='https://upd.emule-security.org/server.met')
    proc, ec = start(binary, root, ports, ed2k=False)
    api_proc = None
    try:
        assert source_file.read_text() == original
        api_proc, endpoint, token = start_api(api_binary, root / 'api', ports[0])

        def request(method, path, body=None, expected=200):
            data = None if body is None else json.dumps(body).encode()
            req = Request(endpoint + path, data, {'Authorization': 'Bearer ' + token,
                          'Content-Type': 'application/json'}, method=method)
            try:
                with build_opener(ProxyHandler({})).open(req, timeout=3) as response:
                    assert response.status == expected, response.status
                    result = response.read()
                    return json.loads(result) if result else None
            except HTTPError as error:
                assert error.code == expected, (error.code, error.read())

        def prefs():
            return request('GET', '/preferences')['servers']

        urls = ['https://one.example/list', 'http://two.example/list']
        request('PATCH', '/preferences', {'servers': {'update_urls': urls + [urls[0]],
                                                      'update_url': 'https://ignored.example/list'}})
        wait_for(proc, lambda: prefs()['update_urls'] == urls, root)
        assert prefs()['update_url'] == urls[0]
        assert source_file.read_text().splitlines() == urls
        request('PATCH', '/preferences', {'servers': {'update_url': 'http://legacy.example/list'}})
        expected = ['http://legacy.example/list', urls[1]]
        wait_for(proc, lambda: prefs()['update_urls'] == expected, root)
        for invalid in ['not-an-array', [17], ['ftp://bad/list'], ['http://host/list\nhttp://other/list'],
                        ['http://host/a\x00hidden']]:
            request('PATCH', '/preferences', {'servers': {'update_urls': invalid}}, expected=400)
            assert source_file.read_text().splitlines() == expected
        for invalid in ['http://alias.example/list\x00hidden', 'http://host/a\nother', 'ftp://host/list']:
            request('PATCH', '/preferences', {'servers': {'update_url': invalid}}, expected=400)
            assert source_file.read_text().splitlines() == expected
        # Exercise the actual EC representation, including duplicate child names and >255 entries.
        many = [f'https://source.example/{i}' for i in range(300)]
        listing = tag(C['EC_TAG_SERVERS_URL_LIST'], children=[string(C['EC_TAG_STRING'], u) for u in many])
        op, result = ec.call(C['EC_OP_SET_PREFERENCES'],
                            [tag(C['EC_TAG_PREFS_SERVERS'], children=[listing])])
        assert op == C['EC_OP_NOOP'], (op, result)
        assert source_file.read_text().splitlines() == many
        wait_for(proc, lambda: prefs()['update_urls'] == many, root)
        # Invalid EC input is rejected without truncating the existing file.
        bad = tag(C['EC_TAG_SERVERS_URL_LIST'], children=[integer(C['EC_TAG_STRING'], 3)])
        op, _ = ec.call(C['EC_OP_SET_PREFERENCES'], [tag(C['EC_TAG_PREFS_SERVERS'], children=[bad])])
        assert op == C['EC_OP_FAILED'], op
        assert source_file.read_text().splitlines() == many
        for bad in [integer(C['EC_TAG_SERVERS_URL_LIST'], 1),
                    string(C['EC_TAG_SERVERS_URL_LIST'], 'https://ignored.example/list')]:
            op, _ = ec.call(C['EC_OP_SET_PREFERENCES'], [tag(C['EC_TAG_PREFS_SERVERS'], children=[bad])])
            assert op == C['EC_OP_FAILED'], op
            assert source_file.read_text().splitlines() == many
        for invalid in ['', 'http:///missing-host', 'http://host/a\x00hidden', 'http://host/list\nother']:
            request('POST', '/servers_update', {'url': invalid}, expected=400)
        op, _ = ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'], [integer(C['EC_TAG_SERVERS_UPDATE_URL'], 1)])
        assert op == C['EC_OP_FAILED'], op
        # Keep only the established route for this action.
        request('POST', '/servers/update', {}, expected=405)
        # A write failure is reported to API clients, preserving the existing filesystem object.
        source_file.unlink()
        source_file.mkdir()
        request('PATCH', '/preferences', {'servers': {'update_urls': urls}}, expected=400)
        assert source_file.is_dir()
        # Reading preferences with an unreadable source path must also remain responsive.
        wait_for(proc, lambda: prefs()['update_urls'] == [], root)
        source_file.rmdir()
        source_file.write_text('\n'.join(many) + '\n')
        request('PATCH', '/preferences', {'servers': {'update_urls': []}})
        wait_for(proc, lambda: prefs()['update_urls'] == [], root)
        assert prefs()['update_url'] == ''
        assert source_file.read_text() == ''
        request('POST', '/servers_update', {}, expected=400)
        op, result = ec.call(C['EC_OP_SERVER_UPDATE_FROM_URL'], [])
        assert op == C['EC_OP_FAILED'], (op, result)
        # The empty list remains empty after restarting, with no legacy key to remigrate.
        ec.sock.close()
        ec = None
        # A completed transfer may still have a queued notification during shutdown.
        temporary = root / 'server.met.download'
        temporary.write_bytes(SERVER_MET)
        stop_daemon(proc)
        assert not temporary.exists(), 'shutdown left a completed server.met download behind'
        proc, ec = start(binary, root, ports, ed2k=False)
        assert source_file.read_text() == ''
    finally:
        if api_proc:
            # amuleapi exits 1 once it has seen its EC connection drop, which stopping the
            # core above can cause.
            stop_daemon(api_proc, check=sys.exc_info()[0] is None, codes=(0, 1))
        if ec:
            ec.sock.close()
        if proc.poll() is None:
            stop_daemon(proc)


def migration_cases(binary, root):
    default = 'https://upd.emule-security.org/server.met'
    cases = [
        ('fresh-default', None, None, default + '\n'),
        ('keep-empty-no-legacy', None, '', ''),
        ('migrate-default-into-empty', default, '', default + '\n'),
        ('keep-comments', default, '# custom sources\nhttp://existing.example/list\n',
         default + '\nhttp://existing.example/list\n'),
        ('default-already-present', default, '# keep comments\n' + default + '\n',
         '# keep comments\n' + default + '\n'),
        ('default-already-second', default, '# keep order\nhttp://existing.example/list\n' + default + '\n',
         '# keep order\nhttp://existing.example/list\n' + default + '\n'),
        ('custom-already-first', 'https://custom.example/list', '# keep comment\nhttps://custom.example/list\n',
         '# keep comment\nhttps://custom.example/list\n'),
        ('custom-first', 'https://custom.example/list', 'http://existing.example/list\n',
         'https://custom.example/list\nhttp://existing.example/list\n'),
        ('custom-deduplicated', 'https://custom.example/list',
         'http://existing.example/list\nhttps://custom.example/list\n',
         'https://custom.example/list\nhttp://existing.example/list\n'),
    ]
    for name, legacy, contents, expected in cases:
        folder = root / name
        folder.mkdir()
        (folder / 'Incoming').mkdir()
        (folder / 'Temp').mkdir()
        source_file = folder / 'addresses.dat'
        if contents is not None:
            source_file.write_text(contents)
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        # ED2K is disabled, so checking the shipped default never contacts the Internet.
        write_amule_conf(folder, ports[0], Port=ports[1], UDPPort=ports[2], Ed2kServersUrl=legacy)
        with running_daemon(binary, folder, ports):
            assert source_file.read_text() == expected, (name, source_file.read_text())
            assert 'Ed2kServersUrl=' not in (folder / 'amule.conf').read_text()


def unreadable_migration_case(binary, root):
    root.mkdir()
    (root / 'Incoming').mkdir()
    (root / 'Temp').mkdir()
    source_file = root / 'addresses.dat'
    original = '# Preserve sources until migration can read them.\nhttps://existing.example/list\n'
    source_file.write_text(original)
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    legacy = 'https://custom.example/list'
    write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], Ed2kServersUrl=legacy)
    source_file.chmod(0)
    try:
        with running_daemon(binary, root, ports):
            assert 'Ed2kServersUrl=' + legacy in (root / 'amule.conf').read_text()
    finally:
        source_file.chmod(0o600)
    assert source_file.read_text() == original
    with running_daemon(binary, root, ports):
        assert source_file.read_text().splitlines() == [legacy, 'https://existing.example/list']
        assert 'Ed2kServersUrl=' not in (root / 'amule.conf').read_text()


def main():
    binary = str(Path(sys.argv[1]).resolve())
    api_binary = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-server-updates-') as temp:
            root = Path(temp)

            def case(name, responses, sources, expected, **kwargs):
                run(binary, root / name, responses, sources, expected, **kwargs)

            if api_binary:
                editor_case(binary, api_binary, root / 'editors')
                case('api-merge-all', {'/first': GOOD, '/last': SECOND}, ['/first', '/last'],
                     ['/first', '/last'], api_binary=api_binary)
                case('api-explicit-once', {'/alternate': GOOD}, ['/backup'], ['/alternate'],
                     url='/alternate', api_binary=api_binary)
                case('api-overlap-snapshot', {'/primary': GOOD, '/backup': GOOD},
                     ['/primary', '/backup'], ['/primary', '/backup'],
                     overlap=True, api_binary=api_binary)
                case('api-explicit-failure', {'/backup': GOOD}, ['/backup'], ['/alternate'],
                     url='/alternate', success=False, api_binary=api_binary)
                return

            migration_cases(binary, root)
            connection_case(binary, root / 'connect-after-sequence', already_connecting=False)
            connection_case(binary, root / 'preserve-pending-login', already_connecting=True)
            if hasattr(os, 'geteuid') and os.geteuid() != 0:
                unreadable_migration_case(binary, root / 'unreadable-migration')
            case('merge-after-success', {'/first': GOOD, '/last': SECOND}, ['/first', '/last'], ['/first', '/last'])
            case('first-run', {'/backup': GOOD}, ['/primary', '/backup'], ['/primary', '/backup'], startup=True)
            case('first-run-merges-all', {'/first': GOOD, '/last': SECOND}, ['/first', '/last'],
                 ['/first', '/last'], startup=True)
            # The startup auto-update replaces the empty-list download, so it must connect too.
            case('auto-update-empty-list-connects', {'/backup': GOOD}, ['/backup'], ['/backup'],
                 startup=True, automatic=True, connects=True)
            case('http-failure', {'/backup': GOOD}, ['/primary', '/backup'], ['/primary', '/backup'])
            case('invalid-download', {'/primary': (200, b'invalid'), '/backup': GOOD},
                 ['/primary', '/backup'], ['/primary', '/backup'])
            case('trailing-data', {'/primary': (200, TRAILING), '/backup': GOOD},
                 ['/primary', '/backup'], ['/primary', '/backup'])
            case('truncated-download', {'/primary': (200, TRUNCATED), '/backup': GOOD},
                 ['/primary', '/backup'], ['/primary', '/backup'])
            case('ordered-merge', {'/first': GOOD, '/last': SECOND},
                 ['/first', '/second', '/last', '/unused'], ['/first', '/second', '/last', '/unused'])
            case('all-fail', {}, ['/first', '/last'], ['/first', '/last'], success=False)
            case('truncated-only', {'/backup': (200, TRUNCATED)}, ['/backup'], ['/backup'], success=False)
            case('invalid-only', {'/backup': (200, b'invalid')}, ['/backup'], ['/backup'], success=False)
            case('missing-sources', {}, [], [], success=False, missing=True)
            case('empty-sources', {}, [], [], success=False)
            case('explicit-once', {'/alternate': GOOD}, ['/backup'], ['/alternate'], url='/alternate')
            case('explicit-fails-without-fallback', {'/backup': GOOD}, ['/backup'], ['/alternate'],
                 url='/alternate', success=False)
            case('deduplicated', {'/primary': GOOD, '/backup': GOOD},
                 ['/primary', '/primary', '# comment', 'ftp://unsupported/list', '/backup', '/backup'],
                 ['/primary', '/backup'])
            case('live-edit', {'/backup': GOOD}, ['/backup'], ['/backup'], reload=True)
            case('overlap-snapshot', {'/primary': GOOD, '/backup': GOOD}, ['/primary', '/backup'],
                 ['/primary', '/backup'], overlap=True)
            case('not-modified-continues', {'/primary': (304, b''), '/backup': GOOD},
                 ['/primary', '/backup'], ['/primary', '/backup'], skipped=True)
            case('invalid-requested-url', {'/backup': GOOD}, ['/backup'], [], url='invalid', success=False)
            case('invalid-sources', {}, ['# comment', 'ftp://unsupported/list', 'invalid'], [], success=False)
            case('migrate-custom-first', {'/custom': GOOD, '/backup': GOOD}, ['/backup', '/custom'],
                 ['/custom', '/backup'], legacy='/custom')
            case('cancel-manual-on-shutdown', {}, ['/primary', '/backup'], ['/primary'],
                 success=False, cancel=True)
            case('cancel-auto-on-shutdown', {}, ['/primary', '/backup'], ['/primary'],
                 success=False, cancel=True, automatic=True)
    retry_port_collisions(attempt)
    print('API/EC source-list regression checks passed' if api_binary else
          'Server-list download and migration regression checks passed')


if __name__ == '__main__':
    main()
