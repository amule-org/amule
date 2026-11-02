#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep partial downloads shared; detach excluded completions and completed renames.

Reusing old paths through the watcher observes stale index entries without exposing
private state. No full reload is allowed between removal and path reuse.
"""
from pathlib import Path
import struct
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon, string, tag
from CancelCompletedIntegrationTest import md4, wait_for
from CompletedFileDeleteIntegrationTest import EC
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared

PARTSIZE = 9728000


def seed_part(root, number, name, first_hash, tail, recover):
    part = root / 'Temp' / f'{number:03}.part'
    part.write_bytes(b'a' * PARTSIZE + tail)
    hashes = first_hash + md4(tail)
    file_hash = md4(hashes)
    encoded = name.encode()
    tags = [b'\x82\x01' + struct.pack('<H', len(encoded)) + encoded,
            b'\x83\x02' + struct.pack('<I', PARTSIZE + len(tail))]
    for mark, value in ((0x09, PARTSIZE), (0x0a, PARTSIZE + len(tail))):
        key = bytes([mark]) + b'0'
        tags.append(b'\x03' + struct.pack('<H', len(key)) + key + struct.pack('<I', value))
    # Recovery rehash discovers the tail already on disk, shares the verified partfile,
    # then completes it. The other file keeps its gap and remains a shared download.
    date = int(part.stat().st_mtime) - (1000 if recover else 0)
    part.with_suffix('.part.met').write_bytes(
        b'\xe0' + struct.pack('<I', date) + file_hash + struct.pack('<H', 2) + hashes
        + struct.pack('<I', len(tags)) + b''.join(tags))
    return file_hash


def rename(ec, file_hash, name):
    assert ec.call('EC_OP_RENAME_FILE', [tag(C['EC_TAG_KNOWNFILE'], file_hash, 9),
                                      string(C['EC_TAG_PARTFILE_NAME'], name)])[0] == C['EC_OP_NOOP']


def run(binary):
    first_hash = md4(b'a' * PARTSIZE)
    with tempfile.TemporaryDirectory(prefix='amule-share-lifecycle-') as scratch:
        root = Path(scratch).resolve()
        incoming, temp = root / 'Incoming', root / 'Temp'
        incoming.mkdir()
        temp.mkdir()
        # Collision renames completion to done(0).txt, which matches the filter.
        (incoming / 'done.txt').write_bytes(b'abc')
        complete_hash = seed_part(root, 1, 'done.txt', first_hash, b'b', True)
        active_hash = seed_part(root, 2, 'active.excl', first_hash, b'c', False)
        (root / 'shareddir.dat').write_text(str(temp) + '\n')
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0,
                         ExcludeSharePatterns='*.part*|*.excl|done(0).txt',
                         AutoRescanSharedDirs=1)
        with running_daemon(binary, root, (ec_port, peer_port), ec_class=EC) as (proc, ec):
            wait_for(proc, 'excluded download shared',
                     lambda: shared(ec).get('active.excl') == active_hash)
            rename(ec, active_hash, 'renamed.excl')
            assert shared(ec).get('renamed.excl') == active_hash
            wait_for(proc, 'completion', lambda: (incoming / 'done(0).txt').exists())
            wait_for(proc, 'excluded completion detached',
                     lambda: complete_hash.hex() not in ec.files(True))
            assert shared(ec).get('done.txt') == md4(b'abc')
            # A stale Temp/done.txt key makes NotifyPathAdded skip this new file.
            (temp / 'done.txt').write_bytes(b'message digest')
            wait_for(proc, 'old completion path reusable',
                     lambda: md4(b'message digest').hex() in ec.files(True))

            # A completed rename must re-key even when the new name is allowed.
            control_hash = md4(b'abc')
            rename(ec, control_hash, 'allowed.txt')
            assert shared(ec).get('allowed.txt') == control_hash
            # Let the rename event drain before reusing its source path, so this
            # checks the index rather than move/create event coalescing.
            (incoming / 'rename-barrier.bin').write_bytes(b'rename barrier')
            wait_for(proc, 'rename events drained',
                     lambda: 'rename-barrier.bin' in shared(ec))
            (incoming / 'done.txt').write_bytes(b'ab')
            wait_for(proc, 'old rename path reusable',
                     lambda: md4(b'ab').hex() in ec.files(True))
            rename(ec, control_hash, 'hidden.excl')
            # The EC operation itself must detach it, without waiting for a rescan.
            assert control_hash.hex() not in ec.files(True), 'excluded completed rename stayed shared'
            assert (incoming / 'hidden.excl').read_bytes() == b'abc'
            (incoming / 'exclude-barrier.bin').write_bytes(b'exclude barrier')
            wait_for(proc, 'excluded rename events drained',
                     lambda: 'exclude-barrier.bin' in shared(ec))
            (incoming / 'allowed.txt').write_bytes(b'a')
            wait_for(proc, 'excluded rename path reusable',
                     lambda: md4(b'a').hex() in ec.files(True))
            assert shared(ec).get('renamed.excl') == active_hash
    print('Partial sharing, completion detachment, completed renames and old path reuse passed')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
