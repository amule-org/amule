#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A loopback eMule peer's mod identity survives EC, REST and daemon restart."""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from AllSearchIntegrationTest import (C, EC, exact, free_ports, retry_port_collisions,
                                      start_daemon, stop_daemon)
from DaemonConfig import daemon_env, write_amule_conf
from DownloadDisplayRatesIntegrationTest import hello


def wait_for(fn):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            value = fn()
            if value:
                return value
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.1)
    raise AssertionError('Timed out waiting for fixture state')


def run(daemon, api, root):
    ec_port, tcp_port, http_port = free_ports(3, peer_index=1)
    daemon_dir, api_dir = root / 'daemon', root / 'api'
    daemon_dir.mkdir()
    api_dir.mkdir()
    write_amule_conf(daemon_dir, ec_port, Port=tcp_port, UDPEnable=0, FilterLanIPs=0)
    (daemon_dir / 'amule.conf').chmod(0o600)
    # Seed credits so the history is saved even though this test exchanges no file data.
    peer_hash = bytearray(range(16))
    peer_hash[5], peer_hash[14] = 14, 111
    peer_hash = bytes(peer_hash)
    credit = peer_hash + struct.pack('<IIIIIHB', 1234, 5678, int(time.time()), 0, 0, 0, 0) + bytes(80)
    assert len(credit) == 119
    (daemon_dir / 'clients.met').write_bytes(b'\x12' + struct.pack('<I', 1) + credit)
    config = api_dir / 'amuleapi.conf'
    config.touch(mode=0o600)
    config.write_text(f'[EC]\nHost=127.0.0.1\nPort={ec_port}\nPassword=regression\n'
                      f'[Server]\nBindAddress=127.0.0.1\nPort={http_port}\n')
    env = daemon_env(root)
    token = ''
    processes = []
    logs = []
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def start_core():
        proc, ec = start_daemon(daemon, daemon_dir, (ec_port, tcp_port),
                                args=('-o',), env=env, console='amuled.log')
        ec.sock.close()
        processes.append(proc)
        return proc

    def start_api():
        log = (root / 'amuleapi.log').open('a')
        logs.append(log)
        proc = subprocess.Popen([str(api), '--config-dir', str(api_dir), '--foreground'],
                                env=env, stdout=log, stderr=log)
        processes.append(proc)
        return proc

    def stop(proc):
        processes.remove(proc)
        stop_daemon(proc, timeout=20)

    def request(path, body=None):
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        req = urllib.request.Request(f'http://127.0.0.1:{http_port}/api/v1' + path,
                                     headers=headers,
                                     data=None if body is None else json.dumps(body).encode())
        with opener.open(req, timeout=3) as reply:
            return json.load(reply)

    def ec_history(mod, version='v0.70b', software=0):
        ec = EC(ec_port)
        try:
            op, tags = ec.call(C['EC_OP_GET_CLIENT_HISTORY'])
            assert op == C['EC_OP_CLIENT_HISTORY']
            key, fields = tags[C['EC_TAG_CLIENT']]
            assert key == peer_hash
            if mod:
                assert fields[C['EC_TAG_CLIENT_MOD_VERSION']][0].rstrip(b'\0').decode() == mod
            else:
                assert C['EC_TAG_CLIENT_MOD_VERSION'] not in fields
            assert fields[C['EC_TAG_CLIENT_SOFT_VER_STR']][0].rstrip(b'\0').decode() == version
            assert fields[C['EC_TAG_CLIENT_SOFTWARE']][0] == software
        finally:
            ec.sock.close()

    def identity(rows, mod, version='v0.70b'):
        return next((row for row in rows if row['user_hash'] == peer_hash.hex()
                     and row.get('client_mod_name') == mod
                     and row.get('software_version') == version), None)

    try:
        core = start_core()
        subprocess.run([str(api), '--config-dir', str(api_dir), '--set-admin-pass', 'mod-test'],
                       env=env, check=True, stdout=subprocess.DEVNULL, timeout=20)
        web = start_api()
        wait_for(lambda: request('/health'))
        token = request('/auth/login?include_token=true', {'password': 'mod-test'})['token']
        # Load history before the hello to exercise reconciliation of a cached old record.
        request('/known_clients')
        cases = [
            ('AI v1.6', 0, (0, 70, 1), False),
            ('eMule Qt 1.0', 0, (0, 70, 1), False),
            ('aMule SVN', 3, (2, 3, 3), False),
            ('', 0, (0, 70, 1), False),
            ('legacy mod', 0, (0, 48, 0), True),
        ]
        for mod, software, (major, minor, update), legacy in cases:
            live_version = 'v0.48' if legacy else ('v2.3.3' if software == 3 else 'v0.70b')
            stored_version = 'v0.48a' if legacy else live_version
            with socket.create_connection(('127.0.0.1', tcp_port), timeout=5) as peer:
                def string_tag(code, value):
                    data = value.encode()
                    return bytes([2, 1, 0, code]) + struct.pack('<H', len(data)) + data

                def send(protocol, opcode, payload):
                    peer.sendall(bytes([protocol]) + struct.pack('<I', len(payload) + 1)
                                 + bytes([opcode]) + payload)

                def advertise(identity):
                    version = None if legacy else ((software << 24) | (major << 17) | (minor << 10) | (update << 7))
                    mod_tag = identity.encode() if identity and not legacy else None
                    send(0xe3, 0x01, hello(b'mod-history-peer', '127.0.0.1', tcp_port,
                         user_hash=peer_hash, version=version, mod=mod_tag))
                    if legacy:
                        # Old peers advertise their mod in the supplementary MuleInfo answer.
                        tags = string_tag(0x55, identity)
                        send(0xc5, 0x02, bytes([0x48, 1]) + struct.pack('<I', 1) + tags)

                advertise(mod)
                wait_for(lambda: identity(request('/clients')['clients'], mod or None, live_version))
                wait_for(lambda: identity(request('/known_clients')['known_clients'], mod or None, live_version))
                ec_history(mod, stored_version, software)
                if mod:
                    # Reuse the same client object. An absent tag must clear the old mod,
                    # and supplementary packets must not count as additional sessions.
                    sessions = request('/known_clients')['known_clients'][0]['session_count']
                    advertise('')
                    wait_for(lambda: identity(request('/clients')['clients'], None, live_version))
                    ec_history('', stored_version, software)
                    advertise(mod)
                    wait_for(lambda: identity(request('/clients')['clients'], mod, live_version))
                    ec_history(mod, stored_version, software)
                    assert request('/known_clients')['known_clients'][0]['session_count'] == sessions
            # Restart without any live peers: the response must come from clients.met.
            stop(web)
            stop(core)
            saved = (daemon_dir / 'clients.met').read_bytes()
            assert saved[124:133] == b'AMULEMD1\x02'
            core = start_core()
            ec_history(mod, stored_version, software)
            web = start_api()
            wait_for(lambda: request('/health'))
            token = request('/auth/login?include_token=true', {'password': 'mod-test'})['token']
            row = wait_for(lambda: identity(request('/known_clients')['known_clients'], mod or None, stored_version))
            assert row['uploaded_bytes_total'] == 1234 and row['downloaded_bytes_total'] == 5678
        # A chat retains the known client across disconnect. Its next outbound hello
        # must preserve the last received identity until the peer actually answers.
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(15)
            peer_port = listener.getsockname()[1]
            address = f'127.0.0.1:{peer_port}'
            version = (70 << 10) | (1 << 7)
            greeting = hello(b'mod-history-peer', '127.0.0.1', peer_port,
                             user_hash=peer_hash, version=version, mod=b'AI v1.6')
            with socket.create_connection(('127.0.0.1', tcp_port), timeout=5) as peer:
                peer.sendall(b'\xe3' + struct.pack('<I', len(greeting) + 1) + b'\x01' + greeting)
                wait_for(lambda: identity(request('/clients')['clients'], 'AI v1.6'))
                request(f'/chats/{address}/messages', {'text': 'retain known peer'})
            wait_for(lambda: any(row['user_hash'] == peer_hash.hex() and row.get('connected') is False
                                 for row in request('/clients')['clients']))
            request(f'/chats/{address}/messages', {'text': 'reconnect known peer'})
            outgoing, _ = listener.accept()
            with outgoing:
                outgoing.settimeout(10)
                header = exact(outgoing, 6)
                assert header[0] == 0xe3 and header[5] == 0x01
                exact(outgoing, struct.unpack('<I', header[1:5])[0] - 1)
                wait_for(lambda: (row if (row := identity(request('/clients')['clients'], 'AI v1.6'))
                                 and row.get('connected') else None))
                # A real hello answer with no mod is authoritative and clears it.
                answer = hello(b'mod-history-peer', '127.0.0.1', peer_port,
                               user_hash=peer_hash, version=version)[1:]
                outgoing.sendall(b'\xe3' + struct.pack('<I', len(answer) + 1) + b'\x4c' + answer)
                wait_for(lambda: identity(request('/clients')['clients'], None))
                ec_history('')
        print('PASS: modern/legacy identity, inbound reset, outbound preservation, stable sessions, EC, REST, restart and credits')
    except Exception:
        for log in logs:
            log.flush()
        for log in (*root.glob('*.log'), *daemon_dir.glob('*.log')):
            print(log.name + ':\n' + log.read_text()[-4000:])
        raise
    finally:
        cleanup_errors = []
        for proc in reversed(processes[:]):
            try:
                stop(proc)
            except Exception as error:
                cleanup_errors.append(error)
        for log in logs:
            log.close()
        if cleanup_errors and sys.exc_info()[0] is None:
            raise cleanup_errors[0]


def run_isolated(daemon, api):
    with tempfile.TemporaryDirectory(prefix='amule-mod-history-') as directory:
        run(daemon, api, Path(directory))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--amuled', type=Path, required=True)
    parser.add_argument('--amuleapi', type=Path, required=True)
    args = parser.parse_args()
    retry_port_collisions(run_isolated, args.amuled.resolve(), args.amuleapi.resolve())
