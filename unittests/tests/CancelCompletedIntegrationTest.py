#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""An EC cancel must not delete a finished download from Incoming.

A finished download stays in the download queue until it is cleared, so `amulecmd cancel all`
and the remote GUI's category Cancel still send EC_OP_PARTFILE_DELETE for it. Its path then
points into Incoming.
"""
from pathlib import Path
import struct
import sys
import tempfile
import time

from AllSearchIntegrationTest import (C, free_ports, retry_port_collisions, running_daemon,
                                      tag, integer)
from DaemonConfig import write_amule_conf

DONE_NAME = 'done.bin'
DONE_DATA = bytes(range(256)) * 64
ACTIVE_HASH = bytes(range(16, 32))
ACTIVE_SIZE = 1 << 20
MET_SIDECARS = ('.bak', '.seeds', '.tmp', '.bak.tmp')


def md4(data):
    """RFC 1320. hashlib offers MD4 only through OpenSSL's legacy provider."""
    def rotl(x, n):
        return ((x << n) | (x >> (32 - n))) & 0xffffffff
    length = len(data) * 8
    data += b'\x80' + b'\0' * ((55 - len(data)) % 64) + struct.pack('<Q', length)
    h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    for off in range(0, len(data), 64):
        x = struct.unpack_from('<16I', data, off)
        a, b, c, d = h
        for i in range(16):
            k, s = i, (3, 7, 11, 19)[i % 4]
            a, b, c, d = d, rotl((a + ((b & c) | (~b & d)) + x[k]) & 0xffffffff, s), b, c
        for i in range(16):
            k, s = (i % 4) * 4 + i // 4, (3, 5, 9, 13)[i % 4]
            a, b, c, d = d, rotl((a + ((b & c) | (b & d) | (c & d)) + x[k] + 0x5a827999) & 0xffffffff, s), b, c
        for i in range(16):
            k, s = (0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)[i], (3, 9, 11, 15)[i % 4]
            a, b, c, d = d, rotl((a + (b ^ c ^ d) + x[k] + 0x6ed9eba1) & 0xffffffff, s), b, c
        h = [(v + n) & 0xffffffff for v, n in zip(h, (a, b, c, d))]
    return struct.pack('<4I', *h)


def part_met(file_hash, name, size, gaps=()):
    """A single-part .part.met. Without gap tags the file loads as fully downloaded."""
    name = name.encode()
    tags = [bytes([0x82, 0x01]) + struct.pack('<H', len(name)) + name,  # FT_FILENAME
            bytes([0x83, 0x02]) + struct.pack('<I', size)]  # FT_FILESIZE
    for i, (start, end) in enumerate(gaps):
        for mark, value in ((0x09, start), (0x0a, end)):  # FT_GAPSTART, FT_GAPEND
            key = bytes([mark]) + str(i).encode()
            tags.append(b'\x03' + struct.pack('<H', len(key)) + key + struct.pack('<I', value))
    return (b'\xe0' + struct.pack('<I', 0) + file_hash + struct.pack('<H', 0)
            + struct.pack('<I', len(tags)) + b''.join(tags))


def wait_for(proc, what, check, timeout=120):
    deadline = time.monotonic() + timeout
    while not check():
        if proc.poll() is not None:
            raise AssertionError(f'daemon exited while waiting for {what}')
        if time.monotonic() > deadline:
            raise AssertionError(f'timed out waiting for {what}')
        time.sleep(0.2)


def run(binary, root):
    for d in ('Incoming', 'Temp'):
        (root / d).mkdir()
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    ec_port, peer_port, udp_port = ports
    write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port)
    done_hash = md4(DONE_DATA)
    (root / 'Temp/001.part').write_bytes(DONE_DATA)
    (root / 'Temp/001.part.met').write_bytes(part_met(done_hash, DONE_NAME, len(DONE_DATA)))
    # Empty data file: a non-empty one with a stale date would be rehashed first.
    (root / 'Temp/002.part').write_bytes(b'')
    (root / 'Temp/002.part.met').write_bytes(
        part_met(ACTIVE_HASH, 'active.bin', ACTIVE_SIZE, [(0, ACTIVE_SIZE)]))

    # A previous interrupted save can leave either temporary file behind.
    for number in ('001', '002'):
        for extension in MET_SIDECARS:
            (root / 'Temp' / f'{number}.part.met{extension}').write_bytes(b'stale metadata')

    # The clean stop checks the exit code: deleting a finished file left it freed in the
    # known list, and the shutdown save then crashed.
    with running_daemon(binary, root, ports) as (proc, ec):
        done = root / 'Incoming' / DONE_NAME
        wait_for(proc, 'the gapless part file to complete',
                 lambda: f'Finished downloading: {DONE_NAME}' in (root / 'logfile').read_text(errors='replace'))
        assert done.read_bytes() == DONE_DATA
        assert not list((root / 'Temp').glob('001.part*')), 'completion left metadata behind'
        # Recreate the interrupted-save artifacts immediately before cancellation.
        for extension in MET_SIDECARS:
            (root / 'Temp' / f'002.part.met{extension}').write_bytes(b'stale metadata')

        # Match amulegui's value-based commands, including its optional instance guard.
        def active():
            _, rows = ec.call(C['EC_OP_GET_DLOAD_QUEUE'])
            ecid, fields = rows[C['EC_TAG_PARTFILE']]
            assert fields[C['EC_TAG_PARTFILE_HASH']][0] == ACTIVE_HASH, fields
            return ecid, fields

        active_id, _ = active()
        def command(opcode, expected_id):
            return ec.call(C[opcode], [tag(C['EC_TAG_PARTFILE'], ACTIVE_HASH, 9,
                [integer(C['EC_TAG_ECID'], expected_id)])])[0]

        for opcode in ('EC_OP_PARTFILE_PAUSE', 'EC_OP_PARTFILE_RESUME',
                       'EC_OP_PARTFILE_STOP', 'EC_OP_PARTFILE_DELETE'):
            assert command(opcode, active_id + 1) == C['EC_OP_FAILED'], opcode
            assert active()[0] == active_id, 'a stale selection removed the download'

        assert command('EC_OP_PARTFILE_PAUSE', active_id) == C['EC_OP_NOOP']
        assert active()[1][C['EC_TAG_PARTFILE_STATUS']][0] == 7  # PS_PAUSED
        assert command('EC_OP_PARTFILE_STOP', active_id) == C['EC_OP_NOOP']
        assert active()[1][C['EC_TAG_PARTFILE_STOPPED']][0] != 0
        assert command('EC_OP_PARTFILE_RESUME', active_id) == C['EC_OP_NOOP']
        _, resumed = active()
        assert resumed[C['EC_TAG_PARTFILE_STATUS']][0] in (0, 1)  # READY / EMPTY
        assert resumed[C['EC_TAG_PARTFILE_STOPPED']][0] == 0
        # A malformed guard must also fail without changing state.
        op, _ = ec.call(C['EC_OP_PARTFILE_PAUSE'], [tag(C['EC_TAG_PARTFILE'], ACTIVE_HASH, 9,
            [tag(C['EC_TAG_ECID'], b'not-an-integer\0', 6)])])
        assert op == C['EC_OP_FAILED']
        assert active()[1][C['EC_TAG_PARTFILE_STATUS']][0] in (0, 1)

        # One packet for both files, as `amulecmd cancel all` sends it.
        op, tags = ec.call(C['EC_OP_PARTFILE_DELETE'],
                           [tag(C['EC_TAG_PARTFILE'], h, 9) for h in (done_hash, ACTIVE_HASH)])
        assert op == C['EC_OP_FAILED'], (op, tags)
        assert DONE_NAME in tags[C['EC_TAG_STRING']][0].decode(), tags
        assert done.exists() and done.read_bytes() == DONE_DATA, 'the finished download was deleted'
        assert not list((root / 'Temp').glob('002.part*')), 'cancellation left part files behind'
        assert command('EC_OP_PARTFILE_PAUSE', active_id) == C['EC_OP_FAILED']

        # Re-adding the same hash creates a new instance. The old selection must not
        # cancel it, but a fresh selection must remove it and all of its metadata.
        link = f'ed2k://|file|active.bin|{ACTIVE_SIZE}|{ACTIVE_HASH.hex()}|/'
        op, _ = ec.call(C['EC_OP_ADD_LINK'],
                        [tag(C['EC_TAG_STRING'], link.encode() + b'\0', 6)])
        assert op == C['EC_OP_NOOP']
        replacement_id, _ = active()
        assert replacement_id != active_id
        assert command('EC_OP_PARTFILE_DELETE', active_id) == C['EC_OP_FAILED']
        assert active()[0] == replacement_id
        assert command('EC_OP_PARTFILE_DELETE', replacement_id) == C['EC_OP_NOOP']
        _, remaining = ec.call(C['EC_OP_GET_DLOAD_QUEUE'])
        assert C['EC_TAG_PARTFILE'] not in remaining, remaining
    assert not list((root / 'Temp').glob('00[12].part*')), 'shutdown recreated removed metadata'


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-cancel-completed-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    print('Cancel-completed checks passed')


if __name__ == '__main__':
    main()
