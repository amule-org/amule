#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Controlled interoperability with the published ed2k-server v0.9.80 binary.

Never connects to a public server. Checks the server's source index via its
loopback admin API, and records the actual client frames through a local relay.
The optional large-library run uses the same path as the smaller fixtures.
"""
import argparse
import base64
import ctypes
import ctypes.util
import hashlib
import json
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zlib
from pathlib import Path

from AllSearchIntegrationTest import C, EC, exact, free_port, string


def wait_for(check, timeout=60):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError('condition timed out')


def json_get(port, route):
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/{route}', timeout=5) as reply:
        return json.load(reply)


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


class Relay:
    def __init__(self, port, plain=False, coalesce=False):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self.target = port
        self.plain = plain
        self.coalesce = coalesce
        self.offers = []
        self.logins = []
        self.sockets = []
        self.errors = []
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        try:
            while True:
                peer, _ = self.listener.accept()
                server = socket.create_connection(('127.0.0.1', self.target))
                self.sockets.extend((peer, server))
                threading.Thread(target=self.copy, args=(peer, server, True), daemon=True).start()
                threading.Thread(target=self.copy, args=(server, peer, False), daemon=True).start()
        except OSError:
            pass

    def copy(self, source, dest, outgoing):
        pending = b''
        try:
            while True:
                header = exact(source, 5)
                protocol, length = struct.unpack('<BI', header)
                assert 0 < length <= 1_000_000, length
                body = exact(source, length)
                if outgoing and body[0] == 0x01:  # OP_LOGINREQUEST
                    payload = body[1:]
                    assert len(payload) >= 26
                    count = struct.unpack_from('<I', payload, 22)[0]
                    request = b'\x03\x0c\x00offerfiles_v\x01\x00\x00\x00'
                    self.logins.append((count, payload.endswith(request)))
                if outgoing and body[0] == 0x15:  # OP_OFFERFILES
                    payload = zlib.decompress(body[1:]) if protocol == 0xd4 else body[1:]
                    count = struct.unpack_from('<I', payload)[0]
                    self.offers.append((time.monotonic(), count, protocol))
                    if self.coalesce:
                        if not pending:
                            pending = header + body
                            continue
                        dest.sendall(pending + header + body)
                        pending = b''
                        continue
                if not outgoing and body[0] == 0x40 and self.plain and len(body) >= 9:
                    flags = struct.unpack_from('<I', body, 5)[0] & ~1
                    body = body[:5] + struct.pack('<I', flags) + body[9:]
                dest.sendall(header + body)
        except (OSError, EOFError):
            pass
        except Exception as error:
            self.errors.append(repr(error))
        finally:
            for peer in (source, dest):
                try:
                    peer.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def close(self):
        self.listener.close()
        for peer in self.sockets:
            peer.close()


def library(root, count, seed):
    incoming = root / 'Incoming'
    incoming.mkdir()
    crypto = ctypes.CDLL(ctypes.util.find_library('crypto'))
    crypto.MD4.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)
    crypto.MD4.restype = ctypes.c_void_p
    records = bytearray(struct.pack('<BI', 0x0e, count))
    aich = bytearray(b'\x02')
    for i in range(count):
        name = f'Linux-fixture-{seed}-{i:06}.bin'
        data = (f'{seed}:{i:06}'.encode() + b'\0' * 64)[:64]
        path = incoming / name
        path.write_bytes(data)
        # Distinct real mtimes keep the startup (size, mtime) lookup index useful;
        # identical fixtures must not turn this publication test into a scan benchmark.
        stamp = 1700000000 + i
        os.utime(path, (stamp, stamp))
        digest = (ctypes.c_ubyte * 16)()
        crypto.MD4(data, len(data), digest)
        # A 64-byte file is one AICH block: its master and sole leaf are SHA-1.
        sha1 = hashlib.sha1(data).digest()
        aich += sha1 + struct.pack('<I', 1) + sha1
        # Real content hashes and mtimes seed known.met to avoid making this
        # publication test depend on asynchronous hashing throughput.
        encoded = name.encode()
        tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(encoded)) + encoded
                + b'\x03\x01\x00\x02' + struct.pack('<I', len(data))
                + b'\x02\x01\x00\x27\x20\x00' + base64.b32encode(sha1))
        records += (struct.pack('<I', int(path.stat().st_mtime)) + bytes(digest)
                    + struct.pack('<HI', 0, 3) + tags)
    (root / 'known.met').write_bytes(records)
    (root / 'known2_64.met').write_bytes(aich)


def daemon(binary, root, count, seed, enabled):
    root.mkdir()
    library(root, count, seed)
    ec_port = free_port()
    (root / 'amule.conf').write_text(f'''[eMule]
Nick={seed}
Port={free_port()}
UDPPort={free_port()}
Address=127.0.0.1
ConnectToKad=0
ConnectToED2K=1
FilterLanIPs=0
NewVersionCheck=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
AddServerListFromServer=0
AddServerListFromClient=0
ExperimentalED2KPublication={int(enabled)}
TempDir={root}/Temp
IncomingDir={root}/Incoming
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={ec_port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
''')
    (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 1, 0))
    log = (root / 'stdout.log').open('w')
    process = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log)
    def connect():
        assert process.poll() is None, (root / 'stdout.log').read_text()[-4000:]
        try:
            return EC(ec_port, timeout=max(60, count / 100))
        except ConnectionRefusedError:
            return None
    try:
        return process, log, wait_for(connect, timeout=max(60, count / 100))
    except BaseException:
        stop(process)
        log.close()
        raise


def connect(ec, port):
    reply = ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{port}')])
    assert reply[0] == C['EC_OP_NOOP'], reply
    assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']


def scenario(args, root, name, *, enabled=True, client_enabled=True, plain=False,
             coalesce=False, soft=100000, batch=200, ceiling=0, clients=1,
             records=1200, reconnect=False):
    root = root / name
    root.mkdir()
    port, admin = free_port(), free_port()
    config = root / 'server.toml'
    config.write_text(f'''[server]
name="OFFERFILES interoperability fixture"
desc="isolated local test"
public=false
this_ip="127.0.0.1"
seed_servers=[]
[network]
tcp_port={port}
listen_ip="127.0.0.1"
max_frame_size=1000000
support_crypt=false
ipv6_enabled=false
[limits]
max_clients=100
max_clients_per_ip=100
soft_limit_files={soft}
hard_limit_files=201
offerfiles_v1={str(enabled).lower()}
offerfiles_batch_max={batch}
offerfiles_min_interval_ms=500
offerfiles_global_records_per_sec={ceiling}
[content_filter]
[admin]
enabled=true
port={admin}
[updates]
enabled=false
[log]
level="info"
''')
    log = (root / 'server.log').open('w')
    server = subprocess.Popen([args.server, '--config', str(config)], cwd=root, stdout=log, stderr=log)
    relay = None
    daemons = []
    try:
        def ready():
            assert server.poll() is None, (root / 'server.log').read_text()[-4000:]
            try:
                return json_get(admin, 'stats')
            except OSError:
                return None
        wait_for(ready)
        relay = Relay(port, plain, coalesce)
        for i in range(clients):
            process, stream, ec = daemon(args.daemon, root / f'client-{i}', records, f'{name}-{i}', client_enabled)
            daemons.append((process, stream, ec))
            connect(ec, relay.port)
        capable = enabled and client_enabled and batch < 201
        expected = min(records, soft) if capable else min(records, 200)
        def indexed():
            rows = json_get(admin, 'clients')
            return rows if len(rows) == clients and all(r['shared_files'] == expected for r in rows) else None
        timeout = max(75, math.ceil(records / 200) * 0.8 + 20)
        if ceiling:
            timeout += clients * records / ceiling
        rows = wait_for(indexed, timeout=timeout)
        if not capable:
            time.sleep(1.3)
            assert len(relay.offers) == clients, relay.offers
        if reconnect:
            previous = len(relay.offers)
            for _, _, ec in daemons:
                assert ec.call(C['EC_OP_SERVER_DISCONNECT'])[0] == C['EC_OP_NOOP']
            wait_for(lambda: not json_get(admin, 'clients'))
            for _, _, ec in daemons:
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
            wait_for(lambda: len(relay.offers) > previous)
            wait_for(indexed)
            assert sum(n for _, n, _ in relay.offers[previous:]) == expected * clients
        indexed_at = time.monotonic()
        stats = json_get(admin, 'stats')
        assert not relay.errors, relay.errors
        assert relay.logins and all(
            login == ((5, True) if client_enabled else (4, False))
            for login in relay.logins), relay.logins
        assert all(0 < n <= 200 and n < 201 for _, n, _ in relay.offers), relay.offers
        if capable:
            assert stats['offer_v1']['oversized'] == 0, stats
            assert stats['offer_over_hard_packets'] == 0, stats
            assert stats['offer_over_soft_records'] == 0, stats
            assert stats['offer_v1']['records'] == expected * clients * (2 if reconnect else 1), stats
            if clients == 1:
                assert all(b[0] - a[0] >= 0.48 for a, b in zip(relay.offers, relay.offers[1:]) if not reconnect), relay.offers
            if ceiling:
                assert stats['offer_v1']['global_waits'] > 0, stats
            if coalesce:
                assert stats['offer_v1']['conn_waits'] > 0, stats
        assert all(p == (0xe3 if plain else 0xd4) for _, _, p in relay.offers), relay.offers
        result = {'scenario': name, 'indexed_per_client': [r['shared_files'] for r in rows],
                  'offer_frames': len(relay.offers), 'first_to_last_seconds': round(relay.offers[-1][0] - relay.offers[0][0], 3),
                  'first_offer_to_indexed_seconds': round(indexed_at - relay.offers[0][0], 3),
                  'offer_v1': stats['offer_v1']}
        print(json.dumps(result), flush=True)
        return result
    finally:
        if sys.exc_info()[0]:
            for path in root.rglob('*.log'):
                print(f'{path.name}:\n{path.read_text(errors="replace")[-4000:]}', file=sys.stderr)
        for process, stream, ec in daemons:
            ec.sock.close()
            stop(process)
            stream.close()
        if relay:
            relay.close()
        stop(server)
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('daemon', type=lambda p: str(Path(p).resolve()))
    parser.add_argument('server', type=lambda p: str(Path(p).resolve()))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--large-library', type=int, default=0)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='amule-offerfiles-interop-') as tmp:
        root = Path(tmp)
        results = []
        cases = [('compressed', {}), ('plain', {'plain': True}),
                 ('coalesced', {'coalesce': True}), ('soft-budget', {'soft': 305}),
                 ('advertised-batch', {'batch': 50, 'records': 305}),
                 ('reconnect', {'records': 600, 'reconnect': True}),
                 ('server-disabled', {'enabled': False}),
                 ('client-disabled', {'client_enabled': False}),
                 ('invalid-policy', {'batch': 201}),
                 ('global-ceiling', {'ceiling': 200}),
                 ('reconnect-wave', {'clients': 4, 'records': 600, 'ceiling': 400, 'reconnect': True})]
        if args.large_library:
            cases.append(('large-library', {'records': args.large_library}))
        for name, options in cases:
            results.append(scenario(args, root, name, **options))
        if args.output:
            args.output.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
