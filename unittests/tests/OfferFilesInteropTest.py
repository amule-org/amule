#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Controlled interoperability with ed2k-server (verified with v0.9.85).

Never connects to a public server. Checks the server's source index via its
loopback admin API, and records the actual client frames through a local relay.

Build the server from its release tag (Rust 1.88 or newer), then run:

    python3 unittests/tests/OfferFilesInteropTest.py AMULED ED2K_SERVER \\
        [--case NAME] [--large-library 60000] [--queue-samples] [--output FILE]

or configure with -DENABLE_INTEGRATION_TESTS=ON -DOFFERFILES_INTEROP_SERVER=PATH
and run `ctest -R OfferFilesInteropTest`. Needs a libcrypto with MD4 for the
known.met fixtures. --queue-samples needs a Debug amuled.
"""
import argparse
import base64
import ctypes
import ctypes.util
import hashlib
import json
import math
import os
import re
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

from DaemonConfig import daemon_env, write_amule_conf
from ShareWatcherMoveIntegrationTest import shared

from AllSearchIntegrationTest import C, EC, exact, free_ports, stop_daemon, string, tag
from OfferFilesInteropPeer import CompletionSource, SearchObserver, login_tags


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
    stop_daemon(process, timeout=15, check=False)


class Relay:
    def __init__(self, port, plain=False, coalesce=False, unsolicited=False, delay_ident=0, ident_before_id=False,
                 drop_ident=False, late_ident=0):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self.target = port
        self.plain = plain
        self.coalesce = coalesce
        self.unsolicited = unsolicited
        self.delay_ident = delay_ident
        self.ident_before_id = ident_before_id
        self.drop_ident = drop_ident
        self.late_ident = late_ident
        self.ident_forwarded_at = None
        self.offers = []
        self.offer_payloads = []
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
        pending_id = b''
        try:
            while True:
                header = exact(source, 5)
                protocol, length = struct.unpack('<BI', header)
                assert 0 < length <= 1_000_000, length
                body = exact(source, length)
                if outgoing and body[0] == 0x01:  # OP_LOGINREQUEST
                    payload = body[1:]
                    self.logins.append(login_tags(payload))
                if not outgoing and body[0] == 0x40 and self.ident_before_id and not pending_id:
                    pending_id = header + body
                    continue
                if not outgoing and body[0] == 0x41 and self.drop_ident:
                    continue  # Lugdunum sends no SERVERIDENT at login.
                if not outgoing and body[0] == 0x41:  # OP_SERVERIDENT
                    if self.unsolicited:
                        payload = zlib.decompress(body[1:]) if protocol == 0xd4 else body[1:]
                        assert len(payload) >= 26
                        def uint32_tag(name, value):
                            raw = bytes([name]) if isinstance(name, int) else name.encode()
                            return b'\x03' + struct.pack('<H', len(raw)) + raw + struct.pack('<I', value)
                        fields = [('offerfiles_v', 1), ('offerfiles_batch_max', 2),
                                  ('offerfiles_min_interval_ms', 500), (0x88, 100000), (0x89, 201)]
                        body = (b'\x41' + payload[:22] + struct.pack('<I', len(fields))
                                + b''.join(uint32_tag(name, value) for name, value in fields))
                        header = struct.pack('<BI', 0xe3, len(body))
                    if self.delay_ident or self.late_ident:
                        time.sleep(self.delay_ident or self.late_ident)
                    self.ident_forwarded_at = time.monotonic()
                    if pending_id:
                        dest.sendall(header + body + pending_id)
                        pending_id = b''
                        continue
                if outgoing and body[0] == 0x15:  # OP_OFFERFILES
                    if self.delay_ident and self.ident_forwarded_at is None:
                        self.errors.append('OFFERFILES sent before the delayed SERVERIDENT')
                    payload = zlib.decompress(body[1:]) if protocol == 0xd4 else body[1:]
                    count = struct.unpack_from('<I', payload)[0]
                    self.offer_payloads.append(payload)
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


def daemon(binary, root, count, seed, enabled, prepare=None, sections=None):
    root.mkdir()
    library(root, count, seed)
    if prepare:
        prepare(root)
    ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
    config_sections = {'Debug': {'Cat_Servers': 1}}
    for name, values in (sections or {}).items():
        config_sections.setdefault(name, {}).update(values)
    write_amule_conf(root, ec_port, Nick=seed, Port=peer_port, UDPPort=udp_port,
                     ConnectToED2K=1, FilterLanIPs=0, GeoIPAutoUpdate=0,
                     GeoIPSource='custom', GeoIPCustomUrl='', VerboseDebug=1,
                     VerboseDebugLogfile=1, ED2KAcceleratedPublication=int(enabled),
                     sections=config_sections)
    (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 1, 0))
    log = (root / 'stdout.log').open('w')
    process = subprocess.Popen([binary, '-c', str(root)], env=daemon_env(root), stdout=log, stderr=log)
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


def connect(ec, port, seeded=False):
    if seeded:
        assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
        return
    reply = ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{port}')])
    assert reply[0] == C['EC_OP_NOOP'], reply
    assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']


def scenario(args, root, name, *, enabled=True, client_enabled=True, plain=False,
             coalesce=False, soft=100000, batch=200, ceiling=0, clients=1,
             records=1200, reconnect=False, republish=False, completion=False,
             unsolicited=False, delay_ident=0, idle_before_connect=0, ident_before_id=False,
             hard=201, seed_legacy_limits=False, drop_ident=False, late_ident=0):
    root = root / name
    root.mkdir()
    port, admin = free_ports(2)
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
hard_limit_files={hard}
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
    samples = []
    observer = None
    download = None
    source_checks = []
    started = time.monotonic()

    def sample():
        stats = json_get(admin, 'stats')['offer_v1']
        queues = []
        for i in range(len(daemons)):
            path = root / f'client-{i}' / 'logfile'
            text = path.read_text(errors='replace') if path.exists() else ''
            matches = re.findall(r'OFFERFILES queue: tick_ms=(\d+) pending_control_packets=(\d+)', text)
            queues.append({'tick_ms': int(matches[-1][0]), 'pending_control_packets': int(matches[-1][1])}
                          if matches else None)
        samples.append({'elapsed_seconds': round(time.monotonic() - started, 3),
                        'client_queues': queues, 'offer_v1': stats})

    try:
        def ready():
            assert server.poll() is None, (root / 'server.log').read_text()[-4000:]
            try:
                return json_get(admin, 'stats')
            except OSError:
                return None
        wait_for(ready)
        relay = Relay(port, plain, coalesce, unsolicited, delay_ident, ident_before_id, drop_ident, late_ident)
        def prepare(client_root):
            nonlocal download
            if completion:
                download = CompletionSource(client_root)
            if seed_legacy_limits:
                tags = (b'\x03\x01\x00\x88' + struct.pack('<I', soft)
                        + b'\x03\x01\x00\x89' + struct.pack('<I', hard)
                        + b'\x02\x01\x00\x85\x09\x00localhost')
                # Numerical loopback addresses are reserved by the server-list parser.
                record = bytes(4) + struct.pack('<HI', relay.port, 3) + tags
                (client_root / 'server.met').write_bytes(b'\xe0' + struct.pack('<I', 1) + record)
        for i in range(clients):
            process, stream, ec = daemon(args.daemon, root / f'client-{i}', records, f'{name}-{i}', client_enabled,
                                         prepare if completion or seed_legacy_limits else None)
            daemons.append((process, stream, ec))
            if idle_before_connect:
                # Let the legacy minute timer become due during the delayed identity.
                end = time.monotonic() + idle_before_connect
                wait_for(lambda: time.monotonic() >= end, timeout=idle_before_connect + 2)
            connected_at = time.monotonic()
            connect(ec, relay.port, seed_legacy_limits)
        capable = enabled and client_enabled and batch < hard and not drop_ident
        expected = min(records + int(completion), soft) if capable else min(records, 200, hard - 1 if seed_legacy_limits else 200)
        def indexed():
            assert not relay.errors, relay.errors
            if relay.offers:
                assert all(n < hard for _, n, _ in relay.offers), 'publication reached the exclusive hard boundary'
            # A late identity follows one legacy batch, which the server skips as oversized.
            negotiated = relay.offers[1:] if late_ident else relay.offers
            if capable and negotiated:
                assert all(n <= min(batch, 200) for _, n, _ in negotiated), \
                    'publication preceded the negotiated packet limit'
            if unsolicited and relay.offers:
                assert all(n == 200 for _, n, _ in relay.offers), \
                    'unsolicited policy replaced the disabled client legacy packet limit'
            if args.queue_samples:
                sample()
            rows = [r for r in json_get(admin, 'clients') if r['nick'] != SearchObserver.nick]
            return rows if len(rows) == clients and all(r['shared_files'] == expected for r in rows) else None
        timeout = max(75, math.ceil(records / 200) * 0.8 + 20)
        if ceiling:
            timeout += clients * records / ceiling
        rows = wait_for(indexed, timeout=timeout)
        if not capable:
            time.sleep(1.3)
            assert len(relay.offers) == clients, relay.offers
        if drop_ident:
            # Without an identity, the first offer waits only briefly after the ID.
            delay = relay.offers[0][0] - connected_at
            assert 4.5 <= delay <= 15, delay
            if unsolicited:
                for client in root.glob('client-*'):
                    text = (client / 'logfile').read_text(errors='replace')
                    assert 'OFFERFILES v1:' not in text and 'OFFERFILES queue:' not in text, \
                        'unsolicited advertisement activated a policy on an opted-out connection'
        refresh_records = 0
        if republish:
            assert capable and clients == 1 and records > soft
            ec = daemons[0][2]
            first_hash = relay.offer_payloads[0][4:20]
            original = next(name for name, digest in shared(ec).items() if digest == first_hash)
            observer = SearchObserver(port)
            initial = observer.search('Linux')[first_hash]
            assert initial[b'\x01'] == original and initial[b'\x15'] == initial[b'\x30'] == 1, initial
            for revision in range(2):
                renamed = f'Linux-renamed-{revision}.bin'
                previous = len(relay.offers)
                assert ec.call(C['EC_OP_RENAME_FILE'], [tag(C['EC_TAG_KNOWNFILE'], first_hash, 9),
                                                       string(C['EC_TAG_PARTFILE_NAME'], renamed)])[0] == C['EC_OP_NOOP']
                wait_for(lambda: len(relay.offers) > previous, timeout=5)
                payload = relay.offer_payloads[previous]
                assert struct.unpack_from('<I', payload)[0] == 1, payload
                assert payload[4:20] == first_hash and renamed.encode() in payload, (original, payload)
                refresh_records += 1
                wait_for(lambda: json_get(admin, 'stats')['offer_v1']['records'] == expected + refresh_records)
                assert indexed(), 'refresh changed the distinct indexed count'
                found = observer.search('Linux')[first_hash]
                assert found[b'\x01'] == original and found[b'\x15'] == found[b'\x30'] == 1, found
                assert first_hash not in observer.search('renamed'), 'rename unexpectedly changed search keywords'
                source_checks.append({'event': 'rename', 'indexed_name': found[b'\x01'],
                                      'sources': found[b'\x15'], 'complete_sources': found[b'\x30']})
            time.sleep(1.3)
            assert len(relay.offers) == previous + 1, relay.offers
        if completion:
            assert capable and clients == 1 and soft == expected == 1
            observer = SearchObserver(port)
            initial = observer.search('completion')[download.file_hash]
            assert initial[b'\x15'] == 1 and initial[b'\x30'] == 0, initial
            assert struct.unpack_from('<IH', relay.offer_payloads[0], 20) == (0xfcfcfcfc, 0xfcfc)
            source_checks.append({'event': 'partial', 'complete_sources': initial[b'\x30']})
            previous = len(relay.offers)
            client_config = (root / 'client-0' / 'amule.conf').read_text()
            peer_port = int(re.search(r'^Port=(\d+)$', client_config, re.MULTILINE)[1])
            download.start(peer_port)
            finished = root / 'client-0' / 'Incoming' / download.name
            def completed():
                assert not download.errors, download.errors
                return finished.exists()
            wait_for(completed, timeout=60)
            assert finished.read_bytes() == download.data, 'download completion changed fixture content'
            wait_for(lambda: len(relay.offers) > previous, timeout=10)
            payload = relay.offer_payloads[previous]
            assert struct.unpack_from('<I', payload)[0] == 1 and payload[4:20] == download.file_hash, payload
            assert struct.unpack_from('<IH', payload, 20) == (0xfbfbfbfb, 0xfbfb)
            def complete_source():
                found = observer.search('completion').get(download.file_hash)
                return found if found and found[b'\x30'] == 1 else None
            found = wait_for(complete_source, timeout=10)
            assert found[b'\x01'] == download.name and found[b'\x15'] == 1, found
            assert indexed(), 'completion changed the distinct indexed count'
            refresh_records = 1
            source_checks.append({'event': 'complete', 'indexed_name': found[b'\x01'],
                                  'sources': found[b'\x15'], 'complete_sources': found[b'\x30']})
            assert not download.errors, download.errors
            time.sleep(1.3)
            assert len(relay.offers) == previous + 1, relay.offers
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
            (login.get(b'offerfiles_v') == 1 if client_enabled else b'offerfiles_v' not in login)
            for login in relay.logins), relay.logins
        assert all(0 < n <= 200 and n < hard for _, n, _ in relay.offers), relay.offers
        if capable:
            assert stats['offer_v1']['oversized'] == (1 if late_ident else 0), stats
            assert stats['offer_over_hard_packets'] == 0, stats
            assert stats['offer_over_soft_records'] == 0, stats
            assert stats['offer_v1']['records'] == expected * clients * (2 if reconnect else 1) + refresh_records, stats
            if clients == 1:
                assert all(b[0] - a[0] >= 0.48 for a, b in zip(relay.offers, relay.offers[1:]) if not reconnect), relay.offers
            if ceiling:
                assert stats['offer_v1']['global_waits'] > 0, stats
            if coalesce:
                assert stats['offer_v1']['conn_waits'] > 0, stats
        if plain:
            assert all(p == 0xe3 for _, _, p in relay.offers), relay.offers
        else:
            # Small packets may remain plain when compression would enlarge them.
            assert all(p in (0xe3, 0xd4) for _, _, p in relay.offers), relay.offers
            if not completion:
                assert any(p == 0xd4 for _, _, p in relay.offers), relay.offers
        queue_summary = None
        if capable and args.queue_samples:
            def drained():
                sample()
                latest = samples[-1]
                return (latest['offer_v1']['global_waiting_now'] == 0
                        and all(q is not None and q['pending_control_packets'] == 0 for q in latest['client_queues']))
            wait_for(drained, timeout=5)
            assert any(q is not None for row in samples for q in row['client_queues']), 'queue diagnostics missing (use Debug daemon)'
            queue_summary = {
                'sampled_peak_packets_per_client': [max(q[i]['pending_control_packets']
                    for row in samples if (q := row['client_queues'])[i] is not None) for i in range(clients)],
                'final_packets_per_client': [q['pending_control_packets'] for q in samples[-1]['client_queues']],
                'final_global_waiting': samples[-1]['offer_v1']['global_waiting_now']}
        result = {'scenario' : name, 'indexed_per_client': [r['shared_files'] for r in rows],
                  'offer_frames': len(relay.offers), 'first_to_last_seconds': round(relay.offers[-1][0] - relay.offers[0][0], 3),
                  'first_offer_to_indexed_seconds': round(indexed_at - relay.offers[0][0], 3),
                  'offer_v1': stats['offer_v1'], 'samples': samples, 'source_checks': source_checks,
                  'queue_summary': queue_summary}
        print(json.dumps({key: value for key, value in result.items() if key != 'samples'}), flush=True)
        return result
    finally:
        if observer:
            observer.close()
        if download:
            download.close()
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
    parser.add_argument('--queue-samples', action='store_true', help='require Debug daemon queue diagnostics')
    parser.add_argument('--case', action='append', help='run only named scenarios')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='amule-offerfiles-interop-') as tmp:
        root = Path(tmp)
        results = []
        cases = [('compressed', {}), ('plain', {'plain': True}),
                 ('coalesced', {'coalesce': True}), ('soft-budget', {'soft': 305}),
                 ('advertised-batch', {'batch': 50, 'records': 305}),
                 ('reconnect', {'records': 600, 'reconnect': True}),
                 ('republish-compressed', {'records': 6, 'soft': 5, 'batch': 2, 'republish': True}),
                 ('republish-plain', {'records': 6, 'soft': 5, 'batch': 2, 'republish': True, 'plain': True}),
                 ('completion', {'records': 0, 'soft': 1, 'batch': 1, 'completion': True}),
                 ('server-disabled', {'enabled': False}),
                 ('client-disabled', {'client_enabled': False}),
                 ('unsolicited-advertisement', {'client_enabled': False, 'unsolicited': True}),
                 ('identity-before-id', {'batch': 50, 'records': 305, 'ident_before_id': True}),
                 ('delayed-advertisement', {'batch': 50, 'records': 305, 'delay_ident': 1.6}),
                 ('no-identity', {'drop_ident': True}),
                 ('late-advertisement', {'batch': 50, 'records': 305, 'late_ident': 8}),
                 ('legacy-hard-boundary', {'enabled': False, 'client_enabled': False, 'hard': 3,
                                           'seed_legacy_limits': True, 'records': 6}),
                 ('invalid-policy', {'batch': 201}),
                 ('global-ceiling', {'ceiling': 200}),
                 ('reconnect-wave', {'clients': 4, 'records': 600, 'ceiling': 400, 'reconnect': True})]
        if args.large_library:
            cases.append(('large-library', {'records': args.large_library}))
        if args.case:
            assert set(args.case) <= {name for name, _ in cases}, args.case
            cases = [(name, options) for name, options in cases if name in args.case]
        for name, options in cases:
            results.append(scenario(args, root, name, **options))
        if args.output:
            args.output.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
