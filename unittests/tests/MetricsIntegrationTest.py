#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline metrics HTTP checks, including an EC fixture that stalls the refresher."""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib

from AllSearchIntegrationTest import (C, connect_daemon, exact, free_ports,
                                      integer, string, tag)
from DaemonConfig import write_amule_conf

TOKEN = '0123456789abcdef' * 4


def request(base, path='/metrics', token=None, method='GET', body=None, headers=None):
    fields = dict(headers or {})
    if token is not None:
        fields['Authorization'] = 'Bearer ' + token
    if body is not None:
        fields['Content-Type'] = 'application/json'
    req = urllib.request.Request(base + path, headers=fields, method=method,
        data=None if body is None else json.dumps(body).encode())
    try:
        reply = urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as error:
        reply = error
    with reply:
        return reply.status, reply.headers, reply.read().decode()


def eventually(fn, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value = fn()
            if value:
                return value
        except OSError:
            pass
        time.sleep(.05)
    raise AssertionError('timed out waiting for fixture')


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


@contextmanager
def api_fixture(binary, root, ec_port, enabled=True, configured=True, token=TOKEN, threshold=30):
    root.mkdir()
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
    with (root / 'api.log').open('w') as log:
        subprocess.run([binary, f'--config-dir={root}', '--set-admin-pass=adminpass'],
                       env=env, stdout=log, stderr=log, check=True)
        config = root / 'amuleapi.conf'
        static = root / 'static'
        static.mkdir()
        (static / 'index.html').write_text('fixture SPA')
        digest = hashlib.md5(b'regression').hexdigest().upper()
        text = config.read_text().replace('FailureWindowSeconds=60\nFailureThreshold=30\nLockoutSeconds=300\n', '')
        text = text.replace('PasswordMD5=', 'PasswordMD5=' + digest, 1)
        text = text.replace('StaticRoot=', f'StaticRoot={static}', 1)
        text = text.replace('BasePath=', 'BasePath=/amule', 1)
        # Explicit opt-in; omission must retain the default-off behavior.
        metrics = '[Metrics]\n' + ('Enabled=1\n' if enabled else '')
        metrics += f'FailureThreshold={threshold}\nFailureWindowSeconds=60\nLockoutSeconds=25\n'
        text = text.replace('[Metrics]\nEnabled=0\nToken=',
                            metrics + 'Token=' + (token if configured else ''))
        config.write_text(text)
        port = free_ports(1)[0]
        base = f'http://127.0.0.1:{port}'
        proc = subprocess.Popen([binary, f'--config-dir={root}', '--host=127.0.0.1',
            f'--port={ec_port}', f'--http-port={port}', '--disable-ec-encryption'],
            env=env, stdout=log, stderr=log)
        try:
            def listening():
                assert proc.poll() is None, (root / 'api.log').read_text()
                return request(base, '/api/v1/health')[0] == 200
            eventually(listening)
            yield base, proc
        except BaseException:
            log.flush()
            print((root / 'api.log').read_text(), file=sys.stderr)
            raise
        finally:
            stop(proc)


class CoreHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1:
                    body = zlib.decompress(body)
                op = body[0]
                self.server.ops.append(op)
                if op == C['EC_OP_AUTH_REQ']:
                    reply, tags = C['EC_OP_AUTH_SALT'], [integer(C['EC_TAG_PASSWD_SALT'], 123)]
                elif op == C['EC_OP_AUTH_PASSWD']:
                    reply, tags = C['EC_OP_AUTH_OK'], [string(C['EC_TAG_SERVER_VERSION'], 'metrics-fixture')]
                else:
                    if self.server.hold.is_set():
                        self.server.entered.set()
                        if not self.server.release.wait(15):
                            return
                    reply, tags = C['EC_OP_NOOP'], []
                    if op == C['EC_OP_STAT_REQ']:
                        reply = C['EC_OP_STATS']
                        if self.server.populated:
                            tags = [tag(C['EC_TAG_STATS_SESSION_SENT_BYTES'], struct.pack('!Q', 1234567890123), 5),
                                    tag(C['EC_TAG_STATS_SESSION_RECEIVED_BYTES'], struct.pack('!Q', 9876543210123), 5),
                                    integer(C['EC_TAG_STATS_ACTIVE_CONNECTIONS'], 7),
                                    integer(C['EC_TAG_STATS_UPTIME_SECONDS'], 42),
                                    tag(C['EC_TAG_CONNSTATE'], b'\x01', 2, [
                                        tag(C['EC_TAG_SERVER'], bytes([1, 2, 3, 4]) + struct.pack('!H', 4661), 8)])]
                    if op == C['EC_OP_GET_UPDATE']:
                        if self.server.populated:
                            tags = [tag(C['EC_TAG_CLIENT'], children=[
                                tag(C['EC_TAG_CLIENT'], struct.pack('!I', 1), 4,
                                    [tag(C['EC_TAG_CLIENT_UPLOAD_STATE'], b'\x00', 2)]),
                                tag(C['EC_TAG_CLIENT'], struct.pack('!I', 2), 4,
                                    [tag(C['EC_TAG_CLIENT_UPLOAD_STATE'], b'\x01', 2)])]),
                                tag(C['EC_TAG_SERVER'], children=[
                                    tag(C['EC_TAG_SERVER'], struct.pack('!I', 1), 4, [
                                        integer(C['EC_TAG_SERVER_IP'], 0x04030201),
                                        tag(C['EC_TAG_SERVER_PORT'], struct.pack('!H', 4661), 3),
                                        integer(C['EC_TAG_SERVER_USERS'], 12345)])])]
                        else:
                            tags = [tag(C['EC_TAG_CLIENT']), tag(C['EC_TAG_SERVER'])]
                    if op == self.server.fail_opcode:
                        reply = C['EC_OP_FAILED']
                payload = bytes([reply]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        except (EOFError, OSError):
            pass


class Core(socketserver.ThreadingTCPServer):
    daemon_threads = True


def fake_core_checks(binary, root):
    with Core(('127.0.0.1', 0), CoreHandler) as core:
        core.ops = []
        core.populated = False
        core.fail_opcode = None
        core.hold, core.entered, core.release = threading.Event(), threading.Event(), threading.Event()
        threading.Thread(target=core.serve_forever, daemon=True).start()
        core.hold.set()
        try:
            with api_fixture(binary, root / 'cached', core.server_address[1]) as (base, _):
                code, headers, text = request(base, token=TOKEN)
                assert code == 200 and 'amule_ec_connected 0\n' in text, (code, text)
                assert 'amule_snapshot_timestamp_seconds' not in text, text
                core.hold.clear()
                core.release.set()
                eventually(lambda: 'amule_ec_connected 1\n' in request(base, token=TOKEN)[2])
                text = request(base, token=TOKEN)[2]
                assert 'amule_transferred_bytes_alltime' not in text, text
                assert 'amule_transferred_bytes_total' not in text, text
                assert 'amule_uptime_seconds' not in text, text
                assert 'amule_active_connections' not in text, text
                assert 'daemon_version="metrics-fixture"' in text, text
                assert 'amule_speed_limit_bytes_per_second' not in text, text
                assert 'amule_shared_files' not in text, text
                print('PASS: startup and older-daemon missing values are omitted')

                core.release.clear()
                core.entered.clear()
                core.hold.set()
                assert core.entered.wait(5), 'refresher did not reach the EC barrier'
                before = len(core.ops)
                for _ in range(25):
                    code, headers, text = request(base, '/amule/metrics', TOKEN)
                    assert code == 200 and 'amule_ec_connected 1\n' in text
                assert len(core.ops) == before, (before, len(core.ops))
                core.hold.clear()
                core.release.set()
                print('PASS: 25 scrapes complete while EC is blocked, with zero extra EC requests')

                core.populated = True
                expected = ('amule_active_uploads 1\n',
                            'amule_transferred_bytes_total{direction="upload"} 1234567890123\n',
                            'amule_transferred_bytes_total{direction="download"} 9876543210123\n',
                            'amule_active_connections 7\n', 'amule_uptime_seconds 42\n',
                            'amule_servers 1\n', 'amule_connected_server_users 12345\n')
                # A fixture transition may occur between STAT_REQ and GET_UPDATE.
                # Wait for a complete tick with the new data in both replies.
                def populated_snapshot():
                    text = request(base, token=TOKEN)[2]
                    return text if all(sample in text for sample in expected) else None
                eventually(populated_snapshot)
                core.populated = False
                def cleared_snapshot():
                    text = request(base, token=TOKEN)[2]
                    return ('amule_active_uploads 0\n' in text and 'amule_servers 0\n' in text
                            and all(name not in text for name in ('amule_connected_server_users',
                                    'amule_transferred_bytes_total', 'amule_uptime_seconds')))
                eventually(cleared_snapshot)
                print('PASS: nonzero cached counts and 64-bit session tags; omissions and removals clear values')


                code, headers, body = request(base, token=TOKEN, method='HEAD')
                assert code == 200 and body == '' and int(headers['Content-Length']) > 0
                assert headers['Cache-Control'] == 'no-store' and 'ETag' not in headers
                assert headers['Content-Type'] == 'text/plain; version=0.0.4; charset=utf-8'
                assert request(base, token=TOKEN, headers={'If-None-Match': '*'})[0] == 200
                assert request(base, token=TOKEN, method='POST')[0] == 405
                assert request(base, headers={'authorization': 'bEaReR ' + TOKEN})[0] == 200
                code, headers, _ = request(base)
                assert code == 401 and headers['WWW-Authenticate'] == 'Bearer realm="metrics"'
                login = request(base, '/api/v1/auth/login?include_token=true', method='POST',
                                body={'password': 'adminpass'})
                jwt = json.loads(login[2])['token']
                assert request(base, token=jwt)[0] == 401
                assert request(base, headers={'Cookie': 'amuleapi_token=' + jwt})[0] == 401
                assert request(base, '/api/v1/status', TOKEN)[0] == 401
                code, api_headers, _ = request(base, '/api/v1/status', jwt)
                assert code == 200 and 'ETag' in api_headers
                assert request(base, '/api/v1/status',
                               headers={'Cookie': 'amuleapi_token=' + jwt})[0] == 200
                print('PASS: BasePath, methods, HEAD, cache policy and dedicated bearer authentication')

                for failed_request in ('EC_OP_STAT_REQ', 'EC_OP_GET_UPDATE', 'EC_OP_GET_PREFERENCES'):
                    core.fail_opcode = C[failed_request]
                    def failed_snapshot():
                        text = request(base, token=TOKEN)[2]
                        return text if 'amule_ec_connected 0\n' in text else None
                    failed = eventually(failed_snapshot, seconds=5)
                    assert 'amule_snapshot_timestamp_seconds ' in failed, failed
                    assert 'amule_transfer_rate_bytes_per_second' not in failed, failed
                    assert 'amule_downloads{' not in failed, failed
                    core.fail_opcode = None
                    eventually(lambda: 'amule_ec_connected 1\n' in request(base, token=TOKEN)[2])
                print('PASS: rejected EC refreshes suppress daemon metrics and recover')

                for _ in range(30):
                    request(base, token='bad')
                code, retry_headers, _ = request(base, token=TOKEN)
                assert code == 429 and int(retry_headers['Retry-After']) > 0
                assert request(base, '/api/v1/status', jwt)[0] == 200
                print('PASS: metrics lockout does not lock out JWT clients')
            with api_fixture(binary, root / 'custom-limit', core.server_address[1], threshold=4) as (base, _):
                for _ in range(4):
                    assert request(base, token='bad')[0] == 401
                code, headers, _ = request(base, token=TOKEN)
                assert code == 429 and 0 < int(headers['Retry-After']) <= 25
                assert request(base, '/api/v1/status', 'invalid-jwt')[0] == 401
                print('PASS: configurable metrics limiter retains independent state')
            with api_fixture(binary, root / 'invalid-token', core.server_address[1], token='bad token') as (base, _):
                assert request(base, '/metrics')[0] == 404
                assert request(base, '/api/v1/health')[0] == 200
                diagnostic = (root / 'invalid-token' / 'api.log').read_text()
                assert 'metrics disabled' in diagnostic and 'bad token' not in diagnostic
                print('PASS: malformed optional credential disables metrics without stopping API')
            with api_fixture(binary, root / 'reverse', core.server_address[1]) as (base, _):
                eventually(lambda: 'amule_ec_connected 1\n' in request(base, token=TOKEN)[2])
                for _ in range(30):
                    request(base, '/api/v1/status', 'invalid-jwt')
                assert request(base, '/api/v1/status', 'invalid-jwt')[0] == 429
                assert request(base, token=TOKEN)[0] == 200
                print('PASS: JWT lockout does not lock out metrics clients')
            with api_fixture(binary, root / 'disabled', core.server_address[1], enabled=False) as (base, _):
                assert request(base, '/')[2] == 'fixture SPA'
                assert request(base, '/metrics')[0] == 404
                assert request(base, '/amule/metrics')[0] == 404
                print('PASS: disabled metrics stays 404 with SPA fallback and BasePath')
            with api_fixture(binary, root / 'unconfigured', core.server_address[1], configured=False) as (base, _):
                assert request(base, '/metrics')[0] == 404
                assert request(base, '/amule/metrics', TOKEN)[0] == 404
                assert request(base, '/api/v1/health')[0] == 200
                print('PASS: enabled without a token starts normally and exposes no metrics')
        finally:
            core.release.set()
            core.shutdown()


def real_daemon_checks(daemon, binary, root):
    core_dir = root / 'core'
    core_dir.mkdir()
    ec, peer, udp = free_ports(3, udp_indices=(2,), peer_index=1)
    write_amule_conf(core_dir, ec, Port=peer, UDPPort=udp, UDPEnable=0,
                     sections={'WebServer': {'Enabled': 0}})
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
    with (root / 'core.log').open('w') as log:
        proc = subprocess.Popen([daemon, '-c', str(core_dir)], env=env, stdout=log, stderr=log)
        try:
            connect_daemon(proc, ec, core_dir, peer).sock.close()
            with api_fixture(binary, root / 'real-api', ec) as (base, _):
                eventually(lambda: 'amule_ec_connected 1\n' in request(base, token=TOKEN)[2])
                code, _, text = request(base, token=TOKEN)
                for expected in ('amule_transferred_bytes_alltime{direction="upload"}',
                                 'amule_transferred_bytes_alltime{direction="download"}',
                                 'amule_transferred_bytes_total{direction="upload"} 0\n',
                                 'amule_transferred_bytes_total{direction="download"} 0\n',
                                 'amule_active_connections 0\n', 'amule_uptime_seconds ',
                                 'amule_active_uploads 0\n', 'amule_servers 0\n',
                                 'daemon_version="',
                                 'amule_speed_limit_bytes_per_second{direction="upload"}',
                                 'amule_shared_files 0\n', 'amule_banned_clients 0\n',
                                 'amule_network_connected{network="ed2k"} 0\n',
                                 'amule_network_connected{network="kad"} 0\n'):
                    assert expected in text, (expected, text)
                assert 'amule_kad_network_users' not in text
                assert 'amule_ed2k_network_users' not in text
                promtool = shutil.which('promtool')
                if promtool:
                    subprocess.run([promtool, 'check', 'metrics', '--lint=none', '--extended'], input=text,
                                   text=True, check=True, capture_output=True)
                    lint = subprocess.run([promtool, 'check', 'metrics'], input=text,
                                          text=True, capture_output=True)
                    # The proposal deliberately reserves _total for session counters.
                    expected_lint = 'amule_transferred_bytes_alltime counter metrics should have \"_total\" suffix'
                    assert lint.returncode == 3, (lint.returncode, lint.stdout, lint.stderr)
                    assert (lint.stdout + lint.stderr).strip() == expected_lint, (lint.stdout, lint.stderr)
                    print('PASS: promtool parses real-daemon output; only the specified alltime suffix lint remains')
                else:
                    print('LIMITATION: promtool unavailable; exposition validator not run')
                print('PASS: existing real-daemon EC statistics without joining public networks')
        except BaseException:
            log.flush()
            print((root / 'core.log').read_text(), file=sys.stderr)
            raise
        finally:
            stop(proc)


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='amule-metrics-') as temporary:
        root = Path(temporary)
        fake_core_checks(str(Path(sys.argv[2]).resolve()), root)
        real_daemon_checks(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()), root)
