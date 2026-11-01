#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Delete disposable completed/shared files through real daemon EC and REST paths.

No public networks: seed verified .part fixtures, let the normal completion path
run, and exercise unlink failures by removing directory write permission.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from AllSearchIntegrationTest import (C, exact, free_ports, integer, retry_port_collisions,
                                      start_daemon, stop_daemon, string, tag)
from DaemonConfig import daemon_env, write_amule_conf


def decode(data, offset, count):
    """Keep repeated tags: file-list replies have one KNOWNFILE per record."""
    entries = []
    for _ in range(count):
        name, kind, length = struct.unpack_from('!HBI', data, offset)
        offset += 7
        children = []
        if name & 1:
            n = struct.unpack_from('!H', data, offset)[0]
            offset += 2
            start = offset
            children, offset = decode(data, offset, n)
            length -= offset - start
        value = data[offset:offset + length]
        offset += length
        if kind in (2, 3, 4, 5):
            value = int.from_bytes(value, 'big')
        entries.append((name >> 1, value, children))
    return entries, offset


def field(tags, name):
    return next((value for key, value, _ in tags if key == C[name]), None)


class EC:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        from ECWire import authenticate
        caps = [tag(C['EC_TAG_CAN_PARTIAL_UPDATE'])] if getattr(self, 'partial', False) else []
        self.sock, op, tags = authenticate(self.sock, hashlib.md5(b'regression').digest(), capabilities=caps)
        assert op == C['EC_OP_AUTH_OK'] and C['EC_TAG_CAN_DELETE_COMPLETE'] in tags, (op, tags)
        if caps: assert C['EC_TAG_CAN_PARTIAL_UPDATE'] in tags
        self.file_cache = {}

    def call(self, op, tags=()):
        payload = bytes([C[op]]) + struct.pack('!H', len(tags)) + b''.join(tags)
        self.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        flags, length = struct.unpack('!II', exact(self.sock, 8))
        assert flags == 0x20, flags
        reply = exact(self.sock, length)
        return reply[0], decode(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]

    def files(self, shared=False):
        if shared:
            _, tags = self.call('EC_OP_GET_SHARED_FILES')
            return {field(children, 'EC_TAG_PARTFILE_HASH').hex(): (ecid, children)
                    for name, ecid, children in tags if name == C['EC_TAG_KNOWNFILE']}
        # GET_DLOAD_QUEUE contains active downloads only. Completed rows ride in
        # GET_UPDATE, where legacy clients merge changed children/alive markers
        # and infer deletion from absence (we advertise no partial-update flag).
        _, tags = self.call('EC_OP_GET_UPDATE', [integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_INC_UPDATE'])])
        current = {}
        for name, ecid, children in tags:
            if name not in (C['EC_TAG_PARTFILE'], C['EC_TAG_KNOWNFILE']):
                continue
            previous = self.file_cache.get(ecid, (name, {}))[1].copy()
            previous.update({key: (key, value, nested) for key, value, nested in children})
            current[ecid] = (name, previous)
        self.file_cache = current
        return {field(list(children.values()), 'EC_TAG_PARTFILE_HASH').hex(): (ecid, list(children.values()))
                for ecid, (name, children) in current.items() if name == C['EC_TAG_PARTFILE']}

    def target(self, file_hash, shared=False):
        ecid, children = self.files(shared)[file_hash]
        return [tag(C['EC_TAG_KNOWNFILE'], bytes.fromhex(file_hash), 9),
                integer(C['EC_TAG_ECID'], ecid),
                tag(C['EC_TAG_KNOWNFILE_PATH'], field(children, 'EC_TAG_KNOWNFILE_PATH'), 6),
                tag(C['EC_TAG_PARTFILE_NAME'], field(children, 'EC_TAG_PARTFILE_NAME'), 6)]


def eventually(fn, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError('timed out waiting for fixture state')


opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http(base, path, token=None, method='GET', body=None):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    request = urllib.request.Request(base + path,
        None if body is None else json.dumps(body).encode(), headers, method=method)
    try:
        response = opener.open(request, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        return response.status, json.loads(data) if data else None


# Standard MD4 test vectors; files are < one eD2k chunk, so this is their file hash.
FIXTURES = [
    (b'a', 'bde52cb31de33e46245e05fbdbd6fb24'),
    (b'abc', 'a448017aaf21d8525fc10ae87aa6729d'),
    (b'message digest', 'd9130a8164549fe818874806e1c7014b'),
    (b'abcdefghijklmnopqrstuvwxyz', 'd79e1c308aa5bbcdeea8ed63df412da9'),
    (b'1234567890' * 8, 'e33b4ddc9c38f2199c3e7b164fcc0536'),
    (b'The quick brown fox jumps over the lazy dog', '1bee69a46ba811185c194762abaeae90'),
    (b'The quick brown fox jumps over the lazy dog.', '2812c6c7136898c51f6f6739ad08750e'),
    (b'ab', 'ec388dd78999dfc7cf4632465693b6bf'),
    # Completes into an excluded name. Incomplete downloads are exempt from the filter.
    (b'The quick brown fox jumps over the lazy dog!', '087bccb2b4007ab7d394dbfacc111c34'),
]
# Matches ExcludeSharePatterns below, so the last fixture is the one that becomes excluded.
EXCLUDED_NAME = 'completed-9.docx'


def seed_part(root, number, content, file_hash, name=None):
    part = root / 'Temp' / f'{number:03}.part'
    part.write_bytes(content)
    filename = (name or f'completed-{number}.txt').encode()
    tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(filename)) + filename +
            b'\x03\x01\x00\x02' + struct.pack('<I', len(content)))
    part.with_suffix('.part.met').write_bytes(
        b'\xe0' + struct.pack('<I', int(part.stat().st_mtime)) + bytes.fromhex(file_hash) +
        struct.pack('<HI', 0, 2) + tags)


def run(daemon, api):
    if os.getuid() == 0:
        print('SKIP: directory permission failure requires an unprivileged user')
        return 77
    with tempfile.TemporaryDirectory(prefix='amule-complete-delete-') as tmp:
        root = Path(tmp)
        incoming = root / 'Incoming'
        incoming.mkdir()
        (root / 'Temp').mkdir()
        for number, (content, file_hash) in enumerate(FIXTURES, 1):
            seed_part(root, number, content, file_hash,
                      EXCLUDED_NAME if number == len(FIXTURES) else None)
        # A complete shared file which was never a download.
        shared_hash = '043f8582f241db351ce627e153e7f0e4'  # MD4(ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789)
        (incoming / 'shared.txt').write_bytes(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789')
        ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port,
                         ExcludeSharePatterns='*.docx')
        env = daemon_env(root)
        processes = []
        ec = None
        with (root / 'api.log').open('w') as api_log:
            try:
                proc, ec = start_daemon(daemon, root, (ec_port, peer_port, udp_port), args=(), env=env,
                                        console='daemon.log', ec_class=EC)
                processes.append(proc)
                eventually(lambda: len(rows := ec.files()) == len(FIXTURES) and
                           all(field(row[1], 'EC_TAG_PARTFILE_STATUS') == 9 for row in rows.values()))
                eventually(lambda: shared_hash in ec.files(True))
                # Malformed input, active files, stale identity/path must never unlink.
                assert ec.call('EC_OP_KNOWNFILE_DELETE')[0] == C['EC_OP_FAILED']
                first = FIXTURES[0][1]
                # Completion must leave the excluded file on disk but out of the shared list.
                excluded_hash = FIXTURES[-1][1]
                assert (incoming / EXCLUDED_NAME).exists()
                eventually(lambda: excluded_hash not in ec.files(True))
                assert first in ec.files(True)
                original = ec.target(first)
                # A stale active cancel must never call delete-this on a completed row.
                assert ec.call('EC_OP_PARTFILE_DELETE', [tag(C['EC_TAG_PARTFILE'], bytes.fromhex(first), 9)])[0] == C['EC_OP_FAILED']
                assert first in ec.files() and first in ec.files(True)
                assert (incoming / 'completed-1.txt').read_bytes() == b'a'
                for bad in ([original[0]], [string(C['EC_TAG_KNOWNFILE'], first), *original[1:]],
                            [original[0], integer(C['EC_TAG_ECID'], 0), *original[2:]],
                            [original[0], integer(C['EC_TAG_ECID'], 0xffffffff), *original[2:]],
                            [*original[:2], string(C['EC_TAG_KNOWNFILE_PATH'], '/wrong'), original[3]],
                            [*original[:3], string(C['EC_TAG_PARTFILE_NAME'], 'wrong.txt')]):
                    assert ec.call('EC_OP_KNOWNFILE_DELETE', bad)[0] == C['EC_OP_FAILED']
                    assert (incoming / 'completed-1.txt').read_bytes() == b'a'
                missing_hash = FIXTURES[4][1]
                missing_target = ec.target(missing_hash)
                (incoming / 'completed-5.txt').unlink()
                assert ec.call('EC_OP_KNOWNFILE_DELETE', missing_target)[0] == C['EC_OP_NOOP']
                assert missing_hash not in ec.files() and missing_hash not in ec.files(True)
                active_hash = '0123456789abcdef0123456789abcdef'
                link = f'ed2k://|file|active.txt|100|{active_hash}|/'
                assert ec.call('EC_OP_ADD_LINK', [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                active = ec.target(active_hash)
                assert ec.call('EC_OP_KNOWNFILE_DELETE', active)[0] == C['EC_OP_FAILED']
                assert active_hash in ec.files()
                stale_active = tag(C['EC_TAG_PARTFILE'], bytes.fromhex(active_hash), 9,
                                   [integer(C['EC_TAG_ECID'], 0xffffffff)])
                assert ec.call('EC_OP_PARTFILE_DELETE', [stale_active])[0] == C['EC_OP_FAILED']
                assert ec.call('EC_OP_PARTFILE_DELETE', [stale_active, tag(C['EC_TAG_PARTFILE'], bytes.fromhex(active_hash), 9)])[0] == C['EC_OP_FAILED']
                assert ec.call('EC_OP_PARTFILE_DELETE', [string(C['EC_TAG_PARTFILE'], active_hash)])[0] == C['EC_OP_FAILED']
                assert active_hash in ec.files()
                # Legacy "cancel all" must skip completed entries and continue
                # cancelling active entries later in the same EC packet.
                batch_hash = 'fedcba9876543210fedcba9876543210'
                batch_link = f'ed2k://|file|batch-active.txt|100|{batch_hash}|/'
                assert ec.call('EC_OP_ADD_LINK', [string(C['EC_TAG_STRING'], batch_link)])[0] == C['EC_OP_NOOP']
                assert ec.call('EC_OP_PARTFILE_DELETE', [
                    tag(C['EC_TAG_PARTFILE'], bytes.fromhex(first), 9),
                    tag(C['EC_TAG_PARTFILE'], bytes.fromhex(batch_hash), 9)])[0] == C['EC_OP_FAILED']
                assert first in ec.files() and batch_hash not in ec.files()
                print('PASS: invalid, stale, and active targets rejected')
                # A failed stat is not ENOENT. With no directory search permission,
                # both unlink and FileExists fail, but the completed row must survive.
                incoming.chmod(0o000)
                try:
                    assert ec.call('EC_OP_KNOWNFILE_DELETE', original)[0] == C['EC_OP_FAILED']
                    assert first in ec.files()
                finally:
                    incoming.chmod(0o755)
                assert (incoming / 'completed-1.txt').read_bytes() == b'a'
                # Prove actual unlink refusal keeps both lists unchanged; retry succeeds.
                incoming.chmod(0o555)
                try:
                    assert ec.call('EC_OP_KNOWNFILE_DELETE', original)[0] == C['EC_OP_FAILED']
                    assert first in ec.files() and first in ec.files(True)
                    assert (incoming / 'completed-1.txt').exists()
                finally:
                    incoming.chmod(0o755)
                assert ec.call('EC_OP_KNOWNFILE_DELETE', original)[0] == C['EC_OP_NOOP']
                assert first not in ec.files() and first not in ec.files(True)
                assert not (incoming / 'completed-1.txt').exists()
                # The operation also covers complete shared files without a completed row.
                assert ec.call('EC_OP_KNOWNFILE_DELETE', ec.target(shared_hash, True))[0] == C['EC_OP_NOOP']
                assert shared_hash not in ec.files(True) and not (incoming / 'shared.txt').exists()
                print('PASS: EC unlink failure, retry, and standalone shared-file deletion')

                api_root = root / 'api'
                api_root.mkdir()
                for role in ('admin', 'guest'):
                    subprocess.run([api, '--config-dir='+str(api_root), f'--set-{role}-pass={role}pass'],
                                   env=env, check=True, stdout=api_log, stderr=api_log)
                config = api_root / 'amuleapi.conf'
                config.write_text(config.read_text().replace('PasswordMD5=', 'PasswordMD5=' + hashlib.md5(b'regression').hexdigest(), 1))
                hp = free_ports(1)[0]
                base = f'http://127.0.0.1:{hp}/api/v1'
                api_proc = subprocess.Popen([api, '--config-dir='+str(api_root), '--host=127.0.0.1',
                    '--port='+str(ec_port), '--http-port='+str(hp)],
                    env=env, stdout=api_log, stderr=api_log)
                processes.append(api_proc)
                def healthy():
                    assert api_proc.poll() is None, (root / 'api.log').read_text()
                    try:
                        return http(base, '/health')[1].get('ec_connected')
                    except OSError:
                        return False
                eventually(healthy)
                admin = http(base, '/auth/login?include_token=true', method='POST', body={'password':'adminpass'})[1]['token']
                guest = http(base, '/auth/login?include_token=true', method='POST', body={'password':'guestpass'})[1]['token']
                second, third, fourth = [h for _, h in FIXTURES[1:4]]
                eventually(lambda: http(base, '/downloads/'+second, admin)[0] == 200)
                assert http(base, '/downloads/'+second, guest, 'DELETE')[0] == 403
                assert http(base, '/downloads', guest, 'DELETE', {'hashes':[second]})[0] == 403
                assert (incoming / 'completed-2.txt').exists()
                # Existing v1 clients must never opt into completed-file deletion implicitly.
                for body in (None, {}, {'expected': {'ecid': 1}}):
                    status, result = http(base, '/downloads/'+second, admin, 'DELETE', body)
                    assert status == 409 and result['error']['code'] == 'download_completed', (status, result)
                status, result = http(base, '/downloads', admin, 'DELETE', {'hashes': [second, third]})
                assert status == 207 and all(r['error']['code'] == 'download_completed' for r in result['results'])
                assert (incoming / 'completed-2.txt').exists() and (incoming / 'completed-3.txt').exists()
                cancel_hash = '1123456789abcdef0123456789abcdef'
                assert ec.call('EC_OP_ADD_LINK', [string(C['EC_TAG_STRING'],
                    f'ed2k://|file|cancel-all.txt|100|{cancel_hash}|/')])[0] == C['EC_OP_NOOP']
                eventually(lambda: http(base, '/downloads/'+cancel_hash, admin)[0] == 200)
                status, result = http(base, '/downloads', admin, 'DELETE', {'hashes': [second, cancel_hash]})
                assert status == 207 and result['results'][0]['error']['code'] == 'download_completed' and result['results'][1]['ok'], (status, result)
                assert cancel_hash not in ec.files() and (incoming / 'completed-2.txt').exists()

                detail = http(base, '/downloads/'+second, admin)[1]
                confirmed_target = {k: detail[k] for k in ('ecid', 'name', 'directory', 'status')}
                for key, wrong in [('ecid', 0xffffffff), ('name', 'old.txt'),
                                   ('directory', '/old'), ('status', 'paused')]:
                    stale = dict(confirmed_target, **{key: wrong})
                    status, result = http(base, '/downloads/'+second, admin, 'DELETE', {'confirmed_target': stale})
                    assert status == 409 and result['error']['code'] == 'selection_changed', (status, result)
                    status, result = http(base, '/downloads', admin, 'DELETE',
                                          {'hashes': [second.upper()], 'confirmed_target': {second.upper(): stale}})
                    assert status == 207 and result['results'][0]['error']['code'] == 'selection_changed', (status, result)
                    assert (incoming / 'completed-2.txt').read_bytes() == b'abc'
                # Validate the whole bulk precondition map before mutating any file.
                for body in ({'hashes': [second, third], 'confirmed_target': {second: confirmed_target}},
                             {'hashes': [second], 'confirmed_target': {second: {'ecid': 1}}}):
                    assert http(base, '/downloads', admin, 'DELETE', body)[0] == 400
                    assert (incoming / 'completed-2.txt').exists()
                assert http(base, '/downloads/'+second, admin, 'DELETE', {'confirmed_target': {}})[0] == 400
                incoming.chmod(0o555)
                try:
                    status, result = http(base, '/downloads/'+second, admin, 'DELETE', {'confirmed_target': confirmed_target})
                    assert status == 400 and result['error']['code'] == 'amuled_rejected', (status, result)
                    assert http(base, '/downloads/'+second, admin)[0] == 200
                    assert http(base, '/shared/'+second, admin)[0] == 200
                finally:
                    incoming.chmod(0o755)
                assert http(base, '/downloads/'+second, admin, 'DELETE', {'confirmed_target': confirmed_target})[0] == 204
                assert http(base, '/downloads/'+second, admin)[0] == 404
                assert http(base, '/shared/'+second, admin)[0] == 404
                assert not (incoming / 'completed-2.txt').exists()
                status, result = http(base, '/downloads', admin, 'DELETE', {'hashes':[third, first], 'confirmed_target': {
                    third: {k: http(base, '/downloads/'+third, admin)[1][k] for k in ('ecid', 'name', 'directory', 'status')},
                    first: confirmed_target}})
                assert status == 207 and result['results'][0]['ok'] and not result['results'][1]['ok'], (status, result)
                assert not (incoming / 'completed-3.txt').exists()
                assert http(base, '/downloads/'+third, admin)[0] == 404
                # A completed row may outlive its canonical shared record. Deleting
                # the old row must preserve a newer same-hash copy and its share.
                sixth = FIXTURES[5][1]
                old_target = ec.target(sixth)
                old_id = ec.files()[sixth][0]
                old_path = incoming / 'completed-6.txt'
                old_path.rename(root / 'saved-sixth.txt')
                (incoming / 'alternate-sixth.txt').write_bytes(FIXTURES[5][0])
                # One shared-list lookup per attempt: a record can vanish between a
                # membership test and the read that follows it when tests run in parallel.
                eventually(lambda: sixth in (shared := ec.files(True)) and shared[sixth][0] != old_id)
                canonical_id = ec.files(True)[sixth][0]
                assert ec.call('EC_OP_KNOWNFILE_DELETE', old_target)[0] == C['EC_OP_NOOP']
                assert ec.files(True)[sixth][0] == canonical_id
                assert sixth not in ec.files()
                assert (incoming / 'alternate-sixth.txt').read_bytes() == FIXTURES[5][0]
                # Rehashing a replacement at the same path creates a new shared
                # record. Deleting that shared record must also clear the older
                # completed transfer row for the very same file.
                seventh = FIXTURES[6][1]
                completed_id = ec.files()[seventh][0]
                replacement = incoming / 'completed-7.txt'
                changed_time = replacement.stat().st_mtime + 10
                replacement.rename(root / 'saved-seventh.txt')
                other_copy = incoming / 'alternate-seventh.txt'
                other_copy.write_bytes(FIXTURES[6][0])
                eventually(lambda: seventh in (shared := ec.files(True)) and shared[seventh][0] != completed_id)
                assert ec.call('EC_OP_KNOWNFILE_DELETE', ec.target(seventh, True))[0] == C['EC_OP_NOOP']
                assert not other_copy.exists() and ec.files()[seventh][0] == completed_id
                assert (root / 'saved-seventh.txt').read_bytes() == FIXTURES[6][0]
                replacement.write_bytes(FIXTURES[6][0])
                os.utime(replacement, (changed_time, changed_time))
                eventually(lambda: seventh in (shared := ec.files(True)) and shared[seventh][0] != completed_id)
                assert ec.files()[seventh][0] == completed_id
                eventually(lambda: seventh in (shared := ec.files(True)) and
                           http(base, '/shared/'+seventh, admin)[1].get('ecid') == shared[seventh][0])
                shared_detail = http(base, '/shared/'+seventh, admin)[1]
                target = {k: shared_detail[k] for k in ('ecid', 'name', 'directory', 'incomplete')}
                assert http(base, '/shared/'+seventh, admin, 'DELETE', {'confirmed_target': target})[0] == 204
                assert http(base, '/downloads/'+seventh, admin)[0] == 404
                assert not replacement.exists() and seventh not in ec.files(True)
                assert seventh not in ec.files(), 'deleting a shared replacement left a completed row'
                # Clear remains separate and preserves the bytes/share.
                assert http(base, '/downloads_clear_completed', admin, 'POST', {'hash':fourth})[0] == 200
                assert (incoming / 'completed-4.txt').exists()
                assert http(base, '/shared/'+fourth, admin)[0] == 200
                # Shared REST deletion works after clear and for files never downloaded.
                # The standalone EC fixture has already been deleted; re-share its bytes.
                (incoming / 'shared.txt').write_bytes(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789')
                eventually(lambda: http(base, '/shared/'+shared_hash, admin)[0] == 200)
                for file_hash, filename in ((fourth, 'completed-4.txt'), (shared_hash, 'shared.txt')):
                    path = '/shared/' + file_hash
                    detail = http(base, path, admin)[1]
                    target = {k: detail[k] for k in ('ecid', 'name', 'directory', 'incomplete')}
                    assert target['ecid'] > 0 and target['incomplete'] is False
                    assert http(base, path, method='DELETE')[0] == 401
                    assert http(base, path, guest, 'DELETE', {'confirmed_target': target})[0] == 403
                    for body in (None, {}, {'confirmed_target': {}}, {'confirmed_target': dict(target, incomplete='false')}):
                        assert http(base, path, admin, 'DELETE', body)[0] == 400
                    for key, wrong in [('ecid', 0xffffffff), ('name', 'old.txt'), ('directory', '/old'), ('incomplete', True)]:
                        status, result = http(base, path, admin, 'DELETE', {'confirmed_target': dict(target, **{key: wrong})})
                        assert status == 409 and result['error']['code'] == 'selection_changed', (status, result)
                        assert (incoming / filename).exists()
                    incoming.chmod(0o555)
                    try:
                        status, result = http(base, path, admin, 'DELETE', {'confirmed_target': target})
                        assert status == 400 and result['error']['code'] == 'amuled_rejected', (status, result)
                        assert http(base, path, admin)[0] == 200 and (incoming / filename).exists()
                    finally:
                        incoming.chmod(0o755)
                    assert http(base, '/shared/'+file_hash.upper(), admin, 'DELETE', {'confirmed_target': target}) == (204, None)
                    assert http(base, path, admin)[0] == 404 and not (incoming / filename).exists()
                # Delete the remaining alternate share without touching the saved copy.
                sixth_detail = http(base, '/shared/'+sixth, admin)[1]
                target = {k: sixth_detail[k] for k in ('ecid', 'name', 'directory', 'incomplete')}
                assert http(base, '/shared/'+sixth, admin, 'DELETE', {'confirmed_target': target})[0] == 204
                assert http(base, '/shared/'+sixth, admin)[0] == 404
                print('PASS: shared REST auth, confirmation, stale targets, failure preservation, cleared and standalone files')
                # A different-hash replacement at the same path is a different file,
                # even when the old completed row still has the confirmed identity.
                eighth = FIXTURES[7][1]
                old_target = ec.target(eighth)
                old_detail = http(base, '/downloads/'+eighth, admin)[1]
                old_confirmation = {k: old_detail[k] for k in ('ecid', 'name', 'directory', 'status')}
                stale_path = incoming / 'completed-8.txt'
                stale_path.rename(root / 'saved-eighth.txt')
                stale_path.write_bytes(FIXTURES[0][0])
                eventually(lambda: first in (shared := ec.files(True)) and
                    field(shared[first][1], 'EC_TAG_PARTFILE_NAME').rstrip(b'\0') == b'completed-8.txt')
                assert ec.call('EC_OP_KNOWNFILE_DELETE', old_target)[0] == C['EC_OP_FAILED'], 'stale completed target deleted a different-hash replacement'
                assert stale_path.read_bytes() == FIXTURES[0][0]
                status, result = http(base, '/downloads/'+eighth, admin, 'DELETE', {'confirmed_target': old_confirmation})
                assert status == 400 and result['error']['code'] == 'amuled_rejected', (status, result)
                assert stale_path.read_bytes() == FIXTURES[0][0]
                assert eighth in ec.files() and first in ec.files(True)
                status, result = http(base, '/downloads', admin, 'DELETE',
                    {'hashes': [eighth], 'confirmed_target': {eighth: old_confirmation}})
                assert status == 207 and result['results'][0]['error']['code'] == 'amuled_rejected', (status, result)
                assert stale_path.read_bytes() == FIXTURES[0][0]

                # Delete the replacement through its own confirmed shared identity.
                eventually(lambda: http(base, '/shared/'+first, admin)[1].get('name') == 'completed-8.txt')
                replacement_detail = http(base, '/shared/'+first, admin)[1]
                target = {k: replacement_detail[k] for k in ('ecid', 'name', 'directory', 'incomplete')}
                assert http(base, '/shared/'+first, admin, 'DELETE', {'confirmed_target': target})[0] == 204
                assert not stale_path.exists() and (root / 'saved-eighth.txt').read_bytes() == FIXTURES[7][0]
                assert ec.call('EC_OP_KNOWNFILE_DELETE', old_target)[0] == C['EC_OP_NOOP']
                print('PASS: stale completed identity preserves a different-hash replacement at the same path')
                # Active cancellation still works with the new per-file EC preconditions.
                active_detail = http(base, '/downloads/'+active_hash, admin)[1]
                assert active_detail['ecid'] > 0
                active_children = ec.files()[active_hash][1]
                part_name = field(active_children, 'EC_TAG_KNOWNFILE_FILENAME').rstrip(b'\0').decode()
                part_path = root / 'Temp' / part_name
                assert part_path.exists()
                assert http(base, '/downloads/'+active_hash, admin, 'DELETE', {})[0] == 204
                assert active_hash not in ec.files()
                assert not part_path.exists() and not Path(str(part_path)+'.met').exists()
                # Deleted complete files can immediately be downloaded again (not cancelled).
                link = f'ed2k://|file|again.txt|1|{first}|/'
                assert ec.call('EC_OP_ADD_LINK', [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                assert first in ec.files()
                print('PASS: REST single/bulk deletion, guest denial, failure preservation, immediate refresh, clear, and re-download')
                stop_daemon(api_proc, timeout=15)
                stop_daemon(proc, timeout=15)
                # Core shutdown persists the known record without adding cancelled hashes.
                known = (root / 'known.met').read_bytes()
                cancelled = (root / 'canceled.met').read_bytes() if (root / 'canceled.met').exists() else b''
                # Shared-file hashing stamps lastSeen, so this record survives the
                # normal known-file TTL pruning at Save(). Just-completed records
                # use the existing completion path's own retention policy.
                assert bytes.fromhex(shared_hash) in known
                for _, file_hash in FIXTURES:
                    assert bytes.fromhex(file_hash) not in cancelled, file_hash
                print('PASS: known-file history retained; complete-file hashes absent from canceled.met')
            except Exception:
                print((root / 'daemon.log').read_text(), file=sys.stderr)
                print((root / 'api.log').read_text(), file=sys.stderr)
                raise
            finally:
                incoming.chmod(0o755)
                if ec:
                    ec.sock.close()
                for child in reversed(processes):
                    stop_daemon(child, timeout=15, check=False)
    return 0


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, *sys.argv[1:]))
