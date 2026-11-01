#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Request-triggered verification through real offline daemon uploads and EC results."""
from contextlib import contextmanager
import configparser
import hashlib
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, exact, free_ports, retry_port_collisions, running_daemon, tag
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import tag_list
from UploadRefillIntegrationTest import packet, send

PART = 9728000


def get_interval(ec):
    op, tags = ec.call(C['EC_OP_GET_PREFERENCES'], [
        tag(C['EC_TAG_SELECT_PREFS'], struct.pack('!I', C['EC_PREFS_FILES']), 4)])
    return tags[C['EC_TAG_PREFS_FILES']][1][C['EC_TAG_FILES_SHARED_REVERIFY_DAYS']][0]

def set_interval(ec, value):
    op, tags = ec.call(C['EC_OP_SET_PREFERENCES'], [
        tag(C['EC_TAG_PREFS_FILES'], children=[
            tag(C['EC_TAG_FILES_SHARED_REVERIFY_DAYS'], struct.pack('!Q', value), 5)])])
    assert op == C['EC_OP_NOOP'], (op, tags)



def records(ec):
    payload = bytes([C['EC_OP_GET_SHARED_FILES']]) + struct.pack('!H', 0)
    ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    _, size = struct.unpack('!II', exact(ec.sock, 8))
    reply = exact(ec.sock, size)
    assert reply[0] == C['EC_OP_SHARED_FILES']
    result = {}
    for name, _, children in tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]:
        if name == C['EC_TAG_KNOWNFILE']:
            fields = {key: value for key, value, _ in children}
            result[fields[C['EC_TAG_PARTFILE_NAME']].rstrip(b'\0').decode()] = fields
    return result


def date(record):
    return int.from_bytes(record.get(C['EC_TAG_KNOWNFILE_VERIFY_DATE'], b''), 'big')


def corrupt(record):
    return any(record.get(C[name], b'').rstrip(b'\0') for name in
               ('EC_TAG_KNOWNFILE_VERIFY_CORRUPT_MD4', 'EC_TAG_KNOWNFILE_VERIFY_CORRUPT_AICH'))


def reports(root):
    return [line for line in (root / 'logfile').read_text(errors='replace').splitlines()
            if 'Verify Local Data (' in line and 'damaged.bin' in line]


def wait(proc, what, check):
    while not check():
        assert proc.poll() is None, f'daemon exited waiting for {what}'
        time.sleep(0.1)


@contextmanager
def daemon(binary, root, interval):
    ec_port, peer_port = free_ports(2, peer_index=1)
    write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0,
                     MaxUpload=1024, SlotAllocation=10, SharedFileReverifyDays=interval,
                     AutoRescanSharedDirs=0)
    with running_daemon(binary, root, (ec_port, peer_port)) as (proc, ec):
        wait(proc, 'initial hashing', lambda: len(records(ec)) == 4)
        yield proc, ec, peer_port


@contextmanager
def peer(port, file_hash, index):
    with socket.socket() as sock:
        address = f'127.0.0.{index + 2}'
        sock.bind((address, 0))
        sock.settimeout(30)
        sock.connect(('127.0.0.1', port))
        name = f'verify-{index}'.encode()
        tags = b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
        hello = (b'\x10' + hashlib.md5(name).digest() + socket.inet_aton(address)
                 + struct.pack('<HI', sock.getsockname()[1], 1) + tags + bytes(6))
        send(sock, 0x01, hello)
        while packet(sock)[1] != 0x4c:
            pass
        send(sock, 0x4f, file_hash)
        send(sock, 0x54, file_hash)
        while packet(sock)[1] != 0x55:
            pass
        yield sock


def request(sock, file_hash, start, length=4096):
    send(sock, 0x47, file_hash + struct.pack('<6I', start, 0, 0, start + length, 0, 0))


def receive(sock, file_hash, content, start, length=4096):
    end = start + length
    while start < end:
        proto, opcode, data = packet(sock)
        if (proto, opcode) != (0xe3, 0x46):
            assert opcode != 0x56, 'healthy part unexpectedly ended the upload session'
            continue
        assert data[:16] == file_hash
        offset, stop = struct.unpack_from('<II', data, 16)
        assert offset == start and stop <= end, (offset, stop, start, end)
        assert data[24:] == content[offset:stop], 'upload payload changed'
        start = stop


def assert_withheld(port, file_hash, index, parts=2, bitmap=2):
    with peer(port, file_hash, index) as sock:
        request(sock, file_hash, 0)
        while True:
            proto, opcode, data = packet(sock)
            assert (proto, opcode) != (0xe3, 0x46), 'corrupt bytes were uploaded'
            if opcode == 0x50:  # OP_FILESTATUS sent when refusing the corrupt part
                assert data[:16] == file_hash
                assert struct.unpack_from('<H', data, 16)[0] == parts
                assert data[18] & ((1 << parts) - 1) == bitmap, 'corrupt part advertised, or healthy part withheld'
                return


def run(binary, root):
    incoming = root / 'Incoming'
    incoming.mkdir()
    (root / 'Temp').mkdir()
    content = os.urandom(PART + 300000)
    damaged = incoming / 'damaged.bin'
    damaged.write_bytes(content)
    (incoming / 'unrequested.bin').write_bytes(os.urandom(100000))
    truncated = incoming / 'truncated.bin'
    truncated.write_bytes(os.urandom(300000))
    fallback = incoming / 'fallback.bin'
    fallback_content = os.urandom(300000)
    fallback.write_bytes(fallback_content)

    print("Checking uploads with automatic verification disabled", flush=True)
    # Disabled by default: a valid upload must not create verification results.
    with daemon(binary, root, None) as (proc, ec, port):
        assert get_interval(ec) == 0, 'default must be off over EC'
        for value in (7, 3650, 0):
            set_interval(ec, value)
            assert get_interval(ec) == value, 'EC preference did not round-trip supported interval'
        file_hash = records(ec)['damaged.bin'][C['EC_TAG_PARTFILE_HASH']]
        with peer(port, file_hash, 0) as sock:
            request(sock, file_hash, PART)
            receive(sock, file_hash, content, PART)
        assert all(not date(record) for record in records(ec).values())
        set_interval(ec, 7)
        for invalid in (3651, 0xFFFFFFFF, 0x100000009):
            set_interval(ec, invalid)
            assert get_interval(ec) == 7, 'invalid EC interval modified the setting'
        set_interval(ec, 3650)
    saved = configparser.ConfigParser()
    saved.read(root / 'amule.conf')
    assert saved.getint('eMule', 'SharedFileReverifyDays') == 3650

    with daemon(binary, root, 3650) as (proc, ec, port):
        assert get_interval(ec) == 3650
    # Invalid signed or wide config values must not wrap into a valid enabled interval.
    for invalid in (-1, 3651, 0x100000007):
        with daemon(binary, root, invalid) as (proc, ec, port):
            assert get_interval(ec) == 0

    print("Checking silent corruption and healthy-part uploads", flush=True)
    # Preserve size and mtime: startup must still share the original identity.
    stamp = damaged.stat()
    with damaged.open('r+b') as output:
        output.write(bytes([content[0] ^ 0xff]))
    os.utime(damaged, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    with daemon(binary, root, 0) as (proc, ec, port):
        set_interval(ec, 7)
        assert get_interval(ec) == 7
        assert records(ec)['damaged.bin'][C['EC_TAG_PARTFILE_HASH']] == file_hash
        with peer(port, file_hash, 1) as sock:
            request(sock, file_hash, PART, length=300001)
            # The subsequent status request is a wire barrier after the malformed range.
            send(sock, 0x4f, file_hash)
            while packet(sock)[1] != 0x50:
                pass
            assert not date(records(ec)['damaged.bin']), 'invalid block triggered verification'
            request(sock, file_hash, PART)
            receive(sock, file_hash, content, PART)
            wait(proc, 'persistable corruption result', lambda: date(records(ec)['damaged.bin']))
            result = records(ec)['damaged.bin']
            assert result[C['EC_TAG_KNOWNFILE_VERIFY_CORRUPT_MD4']].rstrip(b'\0') == b'0'
            assert result[C['EC_TAG_KNOWNFILE_VERIFY_CORRUPT_AICH']].rstrip(b'\0') == b'0:0'
            checked_date = date(result)
            # A later request in the same slot still uploads a healthy part.
            request(sock, file_hash, PART + 4096)
            receive(sock, file_hash, content, PART + 4096)
        assert_withheld(port, file_hash, 2)
        assert not date(records(ec)['unrequested.bin'])

        print("Checking size-mismatch persistence", flush=True)
        # A length mismatch must persist damage rather than merely logging it.
        with truncated.open('r+b') as output:
            output.truncate(100000)
        truncated_hash = records(ec)['truncated.bin'][C['EC_TAG_PARTFILE_HASH']]
        with peer(port, truncated_hash, 6) as sock:
            request(sock, truncated_hash, 0)
            wait(proc, 'size-mismatch result', lambda: date(records(ec)['truncated.bin']))
        truncated_result = records(ec)['truncated.bin']
        assert truncated_result[C['EC_TAG_KNOWNFILE_VERIFY_CORRUPT_MD4']].rstrip(b'\0') == b'0'
        assert_withheld(port, truncated_hash, 7, parts=1, bitmap=0)

        print("Checking MD4 fallback without AICH data", flush=True)
        # No AICH data must still allow the MD4 check and uploads to finish.
        aich_path = root / 'known2_64.met'
        aich_data = aich_path.read_bytes()
        aich_path.write_bytes(b'')
        fallback_hash = records(ec)['fallback.bin'][C['EC_TAG_PARTFILE_HASH']]
        with peer(port, fallback_hash, 3) as sock:
            request(sock, fallback_hash, 0)
            receive(sock, fallback_hash, fallback_content, 0)
            wait(proc, 'MD4-only result', lambda: date(records(ec)['fallback.bin']))
        text = (root / 'logfile').read_text(errors='replace')
        assert 'Result OK' not in text, 'automatic success unexpectedly logged'
        assert not corrupt(records(ec)['fallback.bin']), 'MD4 fallback reported corruption'
        aich_path.write_bytes(aich_data)
        assert len(reports(root)) == 1, 'the same-session check was repeated'

    print("Checking persisted corruption after restart", flush=True)
    # known.met restores the result; recent checks are not requeued after restart.
    with daemon(binary, root, 7) as (proc, ec, port):
        reports_before = reports(root)
        result = records(ec)['damaged.bin']
        assert date(result) == checked_date
        assert result[C['EC_TAG_KNOWNFILE_VERIFY_CORRUPT_MD4']].rstrip(b'\0') == b'0'
        assert_withheld(port, file_hash, 4)
        with peer(port, file_hash, 5) as sock:
            request(sock, file_hash, PART)
            receive(sock, file_hash, content, PART)
        assert date(records(ec)['damaged.bin']) == checked_date
        assert not date(records(ec)['unrequested.bin'])
        assert reports(root) == reports_before, 'recent persisted check was repeated'

        print("Checking manual failure reporting", flush=True)
        previous_reports = len(reports(root))
        assert ec.call(C['EC_OP_VERIFY_LOCAL_DATA'],
                       [tag(C['EC_TAG_KNOWNFILE'], file_hash, 9)])[0] == C['EC_OP_NOOP']
        wait(proc, 'manual failure report', lambda: len(reports(root)) > previous_reports)
        assert 'ERRORS FOUND!' in reports(root)[-1], 'manual corruption report was suppressed'

        print("Checking manual verification after repair", flush=True)
        # Manual verification remains available inside the automatic interval and clears
        # withholding after the original bytes have been restored.
        with damaged.open('r+b') as output:
            output.write(content[:1])
        os.utime(damaged, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        assert ec.call(C['EC_OP_VERIFY_LOCAL_DATA'],
                       [tag(C['EC_TAG_KNOWNFILE'], file_hash, 9)])[0] == C['EC_OP_NOOP']
        wait(proc, 'manual repair verification',
             lambda: not corrupt(records(ec)['damaged.bin']))
        assert any('Result OK' in line for line in reports(root)), 'manual success was not logged'
        with peer(port, file_hash, 8) as sock:
            request(sock, file_hash, 0)
            receive(sock, file_hash, content, 0)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-reverify-') as directory:
            run(binary, Path(directory))
    retry_port_collisions(attempt)
    print('Shared-file re-verification integration checks passed')


if __name__ == '__main__':
    main()
