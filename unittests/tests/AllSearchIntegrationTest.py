#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise AllSearch through EC against an isolated daemon and a loopback eD2k server."""
import hashlib
import os
from pathlib import Path
import queue
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


# Resolve wire constants from the protocol source, so renumbering stays visible.
def load_codes():
    source = Path(__file__).resolve().parents[2] / 'src/libs/ec/abstracts/ECCodes.abstract'
    return {name: int(value, 0) for name, value in re.findall(
        r'^\s*(EC_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$',
        source.read_text(), re.MULTILINE)}


C = load_codes()


def exact(sock, n):
    data = b''
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise EOFError('connection closed')
        data += part
    return data


def tag(name, value=b'', kind=1, children=()):
    nested = b''.join(children)
    return (struct.pack('!HBI', name * 2 + bool(children), kind, len(value) + len(nested))
            + (struct.pack('!H', len(children)) if children else b'') + nested + value)


def string(name, value):
    return tag(name, value.encode() + b'\0', 6)


def integer(name, value):
    return tag(name, struct.pack('!I', value), 4)


def parse_tags(data, offset, count):
    result = {}
    for _ in range(count):
        name, kind, length = struct.unpack_from('!HBI', data, offset)
        offset += 7
        children = {}
        if name & 1:
            n = struct.unpack_from('!H', data, offset)[0]
            offset += 2
            start = offset
            children, offset = parse_tags(data, offset, n)
            length -= offset - start
        value = data[offset:offset + length]
        offset += length
        if kind in (2, 3, 4, 5):
            value = int.from_bytes(value, 'big')
        result[name >> 1] = (value, children)
    return result, offset


class EC:
    def __init__(self, port, host="127.0.0.1", password="regression", timeout=60):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        op, tags = self.call(C['EC_OP_AUTH_REQ'], [string(C['EC_TAG_CLIENT_NAME'], 'AllSearch regression'), string(C['EC_TAG_CLIENT_VERSION'], '1'),
                               tag(C['EC_TAG_PROTOCOL_VERSION'], struct.pack('!H', C['EC_CURRENT_PROTOCOL_VERSION']), 3), tag(C['EC_TAG_CAN_MULTI_SEARCH']), tag(C['EC_TAG_CAN_SEARCH_LIST']), tag(C['EC_TAG_CAN_PARTIAL_SEARCH'])])
        assert op == C['EC_OP_AUTH_SALT'], (op, tags)
        salt = tags[C['EC_TAG_PASSWD_SALT']][0]
        password_hash = hashlib.md5(password.encode()).hexdigest()
        salt_hash = hashlib.md5(f'{salt:X}'.encode()).hexdigest()
        proof = hashlib.md5((password_hash + salt_hash).encode()).digest()
        op, tags = self.call(C['EC_OP_AUTH_PASSWD'], [tag(C['EC_TAG_PASSWD_HASH'], proof, 9)])
        assert op == C['EC_OP_AUTH_OK'] and C['EC_TAG_CAN_SEARCH_ALL'] in tags, (op, tags)

    def call(self, op, tags=()):
        payload = bytes([op]) + struct.pack('!H', len(tags)) + b''.join(tags)
        self.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        flags, length = struct.unpack('!II', exact(self.sock, 8))
        assert flags == 0x20, flags
        reply = exact(self.sock, length)
        return reply[0], parse_tags(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]

    def start(self, query, kind=C['EC_SEARCH_ALL'], wait=False):
        request = tag(C['EC_TAG_SEARCH_TYPE'], bytes([kind]), 2, [string(C['EC_TAG_SEARCH_NAME'], query), string(C['EC_TAG_SEARCH_FILE_TYPE'], '')])
        for _ in range(100 if wait else 1):
            op, tags = self.call(C['EC_OP_SEARCH_START'], [request])
            if op == C['EC_OP_STRINGS']:
                break
            time.sleep(0.1)
        assert op == C['EC_OP_STRINGS'], (op, tags)
        assert C['EC_TAG_SEARCH_KAD_ACTIVE'] in tags, tags
        return tags[C['EC_TAG_SEARCH_ID']][0]

    def progress(self, sid):
        return self.call(C['EC_OP_SEARCH_PROGRESS'], [integer(C['EC_TAG_SEARCH_ID'], sid)])[1]


def connect_daemon(proc, port):
    # The caller/ctest may impose its own deadline. Slow architectures can spend
    # minutes hashing passwords before opening EC; wait while the daemon lives.
    while True:
        if proc.poll() is not None:
            raise RuntimeError('daemon exited')
        try:
            return EC(port)
        except ConnectionRefusedError:
            time.sleep(0.1)


def search_record(name, sources=10):
    name = name.encode()
    tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            + b'\x03\x01\x00\x02' + struct.pack('<I', 4096)
            + b'\x03\x01\x00\x15' + struct.pack('<I', sources)
            + b'\x03\x01\x00\x30' + struct.pack('<I', 3))
    return bytes(range(16)) + struct.pack('<IHI', 0, 0, 4) + tags


def stored_result(name, children=(), networks=None):
    record = search_record(name, max(networks) if networks else 10)
    if networks is not None:
        extra = b''
        for key, value in zip((b'AllSearchEd2kSources', b'AllSearchKadSources'), networks):
            extra += b'\x03' + struct.pack('<H', len(key)) + key + struct.pack('<I', value)
        record = record[:22] + struct.pack('<I', 6) + record[26:] + extra
    # Stored results omit the network record's client IP/port before the tags,
    # then append Kad, directory, client/server endpoints, publish info and lists.
    return (record[:16] + record[22:]
            + struct.pack('<BHIHIHIHH', 0, 0, 0, 0, 0, 0, 0, 0, len(children))
            + b''.join(children))


def stored_search(results, kind=C['EC_SEARCH_ALL']):
    query = b'restored'
    return (struct.pack('<BIIH', 1, 1, 123, len(query)) + query
            + struct.pack('<BQI', kind, int(time.time()), len(results)) + b''.join(results))


def stop_daemon(proc):
    proc.terminate()
    code = proc.wait(timeout=10)
    assert code == 0, f"daemon shutdown failed: {code}"


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-all-search-') as root:
        root = Path(root)
        ec_port = free_port()
        config = f'''[eMule]
Nick=regression
Port={free_port()}
UDPPort={free_port()}
Address=127.0.0.1
ConnectToKad=1
ConnectToED2K=1
FilterLanIPs=0
NewVersionCheck=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
AddServerListFromServer=0
AddServerListFromClient=0
TempDir={root}/Temp
IncomingDir={root}/Incoming
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={ec_port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
'''
        (root / 'amule.conf').write_text(config)
        (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 1, 0))
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'))
        with (root / 'stdout.log').open('w') as log:
            proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
            try:
                ec = connect_daemon(proc, ec_port)
                # No available network: fail without creating a search.
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                op, _ = ec.call(C['EC_OP_SEARCH_START'], [tag(C['EC_TAG_SEARCH_TYPE'], bytes([C['EC_SEARCH_ALL']]), 2, [string(C['EC_TAG_SEARCH_NAME'], 'ubuntu')])])
                assert op == C['EC_OP_FAILED'], op
                # Kad-only fallback must not wait for a nonexistent server response.
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                # A short query cannot use Kad when it is the only network.
                for kind in (C['EC_SEARCH_KAD'], C['EC_SEARCH_ALL']):
                    op, _ = ec.call(C['EC_OP_SEARCH_START'], [tag(C['EC_TAG_SEARCH_TYPE'], bytes([kind]), 2,
                        [string(C['EC_TAG_SEARCH_NAME'], 'go'), string(C['EC_TAG_SEARCH_FILE_TYPE'], '')])])
                    assert op == C['EC_OP_FAILED'], (kind, op)
                sid = ec.start('ubuntu linux')
                state = ec.progress(sid)
                assert state[C['EC_TAG_SEARCH_LIFECYCLE_KIND']][0] == C['EC_SEARCH_ALL'] and state[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1, state
                assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1, state
                assert state[C['EC_TAG_SEARCH_ED2K_ACTIVE']][0] == 0, state
                # Standalone Kad retains its duplicate-target rejection policy.
                op, _ = ec.call(C['EC_OP_SEARCH_START'], [tag(C['EC_TAG_SEARCH_TYPE'], bytes([C['EC_SEARCH_KAD']]), 2,
                    [string(C['EC_TAG_SEARCH_NAME'], 'ubuntu linux'), string(C['EC_TAG_SEARCH_FILE_TYPE'], '')])])
                assert op == C['EC_OP_FAILED'] and ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
                # A busy Kad target with no eD2k fallback must reject the new
                # All search without terminating the existing owner's search.
                op, _ = ec.call(C['EC_OP_SEARCH_START'], [tag(C['EC_TAG_SEARCH_TYPE'], bytes([C['EC_SEARCH_ALL']]), 2,
                    [string(C['EC_TAG_SEARCH_NAME'], 'ubuntu linux'), string(C['EC_TAG_SEARCH_FILE_TYPE'], '')])])
                assert op == C['EC_OP_FAILED'] and ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
                assert ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])[0] == C['EC_OP_MISC_DATA']
                state = ec.progress(sid)
                assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2 and state[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0, state
                # Closing must remove its Kad target so the same keyword can restart.
                sid = ec.start('debian testing')
                assert ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid), tag(C['EC_TAG_SEARCH_CLOSE'])])[0] == C['EC_OP_MISC_DATA']
                sid = ec.start('debian testing')
                assert ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
                ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                # Loopback server checks that All keeps the full eD2k query.
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0))
                    listener.listen(1)
                    listener.settimeout(10)
                    port = listener.getsockname()[1]
                    queries = queue.Queue()
                    answer = threading.Event()
                    answer.set()
                    def serve():
                        try:
                            with listener.accept()[0] as peer:
                                peer.settimeout(10)
                                while True:
                                    proto, length = struct.unpack('<BI', exact(peer, 5))
                                    packet = exact(peer, length)
                                    if packet[0] == 1:  # OP_LOGINREQUEST
                                        body = b'\x40' + struct.pack('<II', 0x12345678, 0)
                                        peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
                                    elif packet[0] == 0x16:  # OP_SEARCHREQUEST
                                        queries.put(packet[1:])
                                        if not answer.wait(10):
                                            return
                                        sources = 0xffffffff if b'overflow' in packet else 10
                                        body = (b'\x33' + struct.pack('<I', 3)
                                                + search_record('regression.bin', sources)
                                                + search_record('variant.bin')
                                                + search_record('variant.bin'))
                                        peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
                        except (EOFError, OSError):
                            pass
                    worker = threading.Thread(target=serve, daemon=True)
                    worker.start()
                    assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{port}')])[0] == C['EC_OP_NOOP']
                    assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                    sid = ec.start('ubuntu linux', wait=True)
                    query = queries.get(timeout=5)
                    assert b'ubuntu' in query and b'linux' in query, query
                    assert ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])
                    # Both components: eD2k completion cannot finish the Kad component.
                    assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                    # Failed replacements and independent Kad searches must leave
                    # the current eD2k request waiting for its server response.
                    answer.clear()
                    pending = ec.start('pending validation', kind=C['EC_SEARCH_LOCAL'])
                    queries.get(timeout=5)
                    for kind in (C['EC_SEARCH_GLOBAL'], C['EC_SEARCH_ALL']):
                        op, _ = ec.call(C['EC_OP_SEARCH_START'], [tag(C['EC_TAG_SEARCH_TYPE'], bytes([kind]), 2,
                            [string(C['EC_TAG_SEARCH_NAME'], '('), string(C['EC_TAG_SEARCH_FILE_TYPE'], '')])])
                        assert op == C['EC_OP_FAILED'], (kind, op)
                        assert ec.progress(pending)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    independent = ec.start('independent regression', kind=C['EC_SEARCH_KAD'])
                    assert ec.progress(pending)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], independent), tag(C['EC_TAG_SEARCH_CLOSE'])])
                    assert ec.progress(pending)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    answer.set()
                    for _ in range(50):
                        state = ec.progress(pending)
                        if state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2:
                            break
                        time.sleep(0.1)
                    assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2 and state[C['EC_TAG_SEARCH_RESULT_COUNT']][0] == 1, state
                    local_counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], pending)])[1][C['EC_TAG_SEARCHFILE']][1]
                    assert C['EC_TAG_SEARCHFILE_ED2K_SOURCES'] not in local_counts and C['EC_TAG_SEARCHFILE_KAD_SOURCES'] not in local_counts, local_counts
                    # Kad's minimum keyword length must not block eD2k fallback.
                    short = ec.start('go')
                    assert b'go' in queries.get(timeout=5)
                    state = ec.progress(short)
                    assert state[C['EC_TAG_SEARCH_LIFECYCLE_KIND']][0] == C['EC_SEARCH_ALL'] and state[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0, state
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], short)])
                    # ALL falls back to eD2k without preempting another client's Kad search.
                    previous = ec.start('fedora workstation', kind=C['EC_SEARCH_KAD'])
                    unrelated = ec.start('debian regression', kind=C['EC_SEARCH_KAD'])
                    op, _ = ec.call(C['EC_OP_SEARCH_START'], [tag(C['EC_TAG_SEARCH_TYPE'], bytes([C['EC_SEARCH_ALL']]), 2,
                        [string(C['EC_TAG_SEARCH_NAME'], 'fedora ('), string(C['EC_TAG_SEARCH_FILE_TYPE'], '')])])
                    assert op == C['EC_OP_FAILED'], op
                    assert ec.progress(previous)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    other_client = EC(ec_port)
                    try:
                        sid = other_client.start('fedora workstation')
                    finally:
                        other_client.sock.close()
                    assert ec.progress(previous)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    assert ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0
                    assert ec.progress(unrelated)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], unrelated)])
                    query = queries.get(timeout=5)
                    assert b'fedora' in query and b'workstation' in query, query
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], previous)])
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])
                    sid = ec.start('combined progress')
                    queries.get(timeout=5)
                    time.sleep(2)
                    state = ec.progress(sid)
                    assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1 and state[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1, state
                    assert 0 < state[C['EC_TAG_SEARCH_LIFECYCLE_PERCENT']][0] < 100, state
                    # eD2k has finished while Kad keeps the combined lifecycle running.
                    assert state[C['EC_TAG_SEARCH_ED2K_ACTIVE']][0] == 0, state
                    # Finishing another Kad search must not complete this combined one.
                    other = ec.start('opensuse tumbleweed', kind=C['EC_SEARCH_KAD'])
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], other)])
                    assert ec.progress(sid)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])
                    assert ec.progress(sid)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2
                    # Reverse completion order: hold the server answer while Kad stops.
                    answer.clear()
                    sid = ec.start('alpine linux')
                    queries.get(timeout=5)
                    time.sleep(1.1)
                    waiting = ec.progress(sid)
                    assert waiting[C['EC_TAG_SEARCH_LIFECYCLE_PERCENT']][0] == 0, waiting
                    assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                    state = ec.progress(sid)
                    assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 1 and state[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0, state
                    assert state[C['EC_TAG_SEARCH_ED2K_ACTIVE']][0] == 1, state
                    answer.set()
                    for _ in range(50):
                        state = ec.progress(sid)
                        if state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2:
                            break
                        time.sleep(0.1)
                    assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2, state
                    assert state[C['EC_TAG_SEARCH_RESULT_COUNT']][0] == 1, state
                    counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], sid)])[1][C['EC_TAG_SEARCHFILE']][1]
                    assert counts[C['EC_TAG_PARTFILE_SOURCE_COUNT']][0] == 30 and counts[C['EC_TAG_PARTFILE_SOURCE_COUNT_XFER']][0] == 9, counts
                    assert counts[C['EC_TAG_SEARCHFILE_ED2K_SOURCES']][0] == 30 and counts[C['EC_TAG_SEARCHFILE_KAD_SOURCES']][0] == 0, counts
                    # Full snapshots include both counts even after earlier polls.
                    counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], sid)])[1][C['EC_TAG_SEARCHFILE']][1]
                    assert counts[C['EC_TAG_SEARCHFILE_ED2K_SOURCES']][0] == 30 and counts[C['EC_TAG_SEARCHFILE_KAD_SOURCES']][0] == 0, counts
                    # Oversized server reports must not wrap when filename
                    # variants are grouped, including through EC serialization.
                    overflow = ec.start('overflow regression')
                    queries.get(timeout=5)
                    for _ in range(50):
                        if ec.progress(overflow)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2:
                            break
                        time.sleep(0.1)
                    counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], overflow)])[1][C['EC_TAG_SEARCHFILE']][1]
                    assert counts[C['EC_TAG_PARTFILE_SOURCE_COUNT']][0] == 0xffffffff, counts
                    assert counts[C['EC_TAG_SEARCHFILE_ED2K_SOURCES']][0] == 0xffffffff, counts
                    # Close while a server response is in flight. Its late results
                    # must not recreate the removed bucket.
                    answer.clear()
                    closed = ec.start('lateclose regression', kind=C['EC_SEARCH_LOCAL'])
                    queries.get(timeout=5)
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], closed), tag(C['EC_TAG_SEARCH_CLOSE'])])
                    answer.set()
                    time.sleep(0.2)
                    state = ec.progress(closed)
                    assert C['EC_TAG_SEARCH_EXPIRED'] in state, state
                    ec.sock.close()
                # Persist and reload an All search: its finished Kad marker must not
                # make it appear to be a standalone Kad search after restart.
                stop_daemon(proc)
                worker.join(timeout=2)
                proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
                ec = connect_daemon(proc, ec_port)
                state = ec.progress(sid)
                assert state[C['EC_TAG_SEARCH_LIFECYCLE_KIND']][0] == C['EC_SEARCH_ALL'], state
                assert state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2 and state[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0, state
                counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], sid)])[1][C['EC_TAG_SEARCHFILE']][1]
                assert counts[C['EC_TAG_PARTFILE_SOURCE_COUNT']][0] == 30 and counts[C['EC_TAG_PARTFILE_SOURCE_COUNT_XFER']][0] == 9, counts
                assert counts[C['EC_TAG_SEARCHFILE_ED2K_SOURCES']][0] == 30 and counts[C['EC_TAG_SEARCHFILE_KAD_SOURCES']][0] == 0, counts
                # Repeated close/restart and bulk shutdown exercise registry ownership.
                ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid), tag(C['EC_TAG_SEARCH_CLOSE'])])
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                for i in range(30):
                    current = ec.start(f'ownership{i} regression', kind=C['EC_SEARCH_KAD'])
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], current), tag(C['EC_TAG_SEARCH_CLOSE'])])
                    current = ec.start(f'ownership{i} regression', kind=C['EC_SEARCH_KAD'])
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], current), tag(C['EC_TAG_SEARCH_CLOSE'])])
                active = [ec.start(f'bulkownership{i} regression', kind=C['EC_SEARCH_KAD']) for i in range(20)]
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                for current in active:
                    assert ec.progress(current)[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                for i in range(20):
                    ec.start(f'shutdownownership{i} regression', kind=C['EC_SEARCH_KAD'])
                ec.sock.close()
                stop_daemon(proc)
                # Validate the fixture first, then truncate after a completed root
                # and after a completed child. ASan/LSan checks cleanup on both paths.
                first = stored_result('first.bin')
                second = stored_result('second.bin')
                valid = stored_search([first, second])
                nested = stored_search([stored_result('parent.bin', [first, second])])
                mixed = stored_search([stored_result('mixed.bin', networks=(10, 50))])
                for fixture, expected, networks in ((valid, 2, None), (mixed, 1, (10, 50)),
                        (valid[:-1], 0, None), (nested[:-1], 0, None),
                        *((stored_search([stored_result('single.bin', networks=(10, 0))], kind), 1, None)
                          for kind in (C['EC_SEARCH_LOCAL'], C['EC_SEARCH_GLOBAL'], C['EC_SEARCH_KAD']))):
                    (root / 'StoredSearches.met').write_bytes(fixture)
                    proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
                    ec = connect_daemon(proc, ec_port)
                    if expected:
                        assert ec.progress(123)[C['EC_TAG_SEARCH_RESULT_COUNT']][0] == expected
                        # Legacy saved ALL results contain only an aggregate, not
                        # a reliable network split. Do not invent E/K counts.
                        counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], 123)])[1][C['EC_TAG_SEARCHFILE']][1]
                        if networks is None:
                            assert C['EC_TAG_SEARCHFILE_ED2K_SOURCES'] not in counts and C['EC_TAG_SEARCHFILE_KAD_SOURCES'] not in counts, counts
                        else:
                            assert (counts[C['EC_TAG_SEARCHFILE_ED2K_SOURCES']][0], counts[C['EC_TAG_SEARCHFILE_KAD_SOURCES']][0]) == networks, counts
                            assert counts[C['EC_TAG_PARTFILE_SOURCE_COUNT']][0] == max(networks), counts
                            # The union seeds the pair once; idle partial polls
                            # must then contain no result tags at all.
                            counts = ec.call(C['EC_OP_SEARCH_RESULTS'], [tag(C['EC_TAG_DETAIL_LEVEL'], bytes([C['EC_DETAIL_INC_UPDATE']]), 2)])[1][C['EC_TAG_SEARCHFILE']][1]
                            assert (counts[C['EC_TAG_SEARCHFILE_ED2K_SOURCES']][0], counts[C['EC_TAG_SEARCHFILE_KAD_SOURCES']][0]) == networks, counts
                            idle = ec.call(C['EC_OP_SEARCH_RESULTS'], [tag(C['EC_TAG_DETAIL_LEVEL'], bytes([C['EC_DETAIL_INC_UPDATE']]), 2)])[1]
                            assert C['EC_TAG_SEARCHFILE'] not in idle, idle
                    else:
                        listing = ec.call(C['EC_OP_SEARCH_LIST'])
                        assert not listing[1], listing
                    ec.sock.close()
                    stop_daemon(proc)
                    saved = (root / 'StoredSearches.met').read_bytes()
                    for name in (b'AllSearchEd2kSources', b'AllSearchKadSources'):
                        assert (name in saved) == (networks is not None), name
                print('PASS: network fallback, query encoding, both completion orders, '
                      'duplicate targets, stop/close, persistence, truncated restore, '
                      'ownership stress, clean shutdown')
            except BaseException:
                log.flush()
                print((root / 'stdout.log').read_text(), file=sys.stderr)
                if (root / 'logfile').exists():
                    print((root / 'logfile').read_text()[-8000:], file=sys.stderr)
                raise
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
