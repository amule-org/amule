#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deleting downloads keeps the shared-file path index in step.

A download with no verified part was never shared, so its removal has no index entry to
erase. A shared download renamed before its removal must have been re-keyed.
"""
from pathlib import Path
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon, tag
from CancelCompletedIntegrationTest import md4, part_met, wait_for
from CompletedFileDeleteIntegrationTest import EC
from DaemonConfig import write_amule_conf
from ShareExclusionLifecycleIntegrationTest import PARTSIZE, rename, seed_part
from ShareWatcherMoveIntegrationTest import shared

EMPTY_HASH = bytes(range(16, 32))
EMPTY_SIZE = 1 << 20
DESYNC = 'Path index: no entry under'


def delete(ec, file_hash):
    assert ec.call('EC_OP_PARTFILE_DELETE', [tag(C['EC_TAG_PARTFILE'], file_hash, 9)])[0] == \
        C['EC_OP_NOOP']


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-download-path-index-') as scratch:
        root = Path(scratch).resolve()
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        (root / 'Incoming' / 'kept.bin').write_bytes(b'abc')
        shared_hash = seed_part(root, 1, 'active.bin', md4(b'a' * PARTSIZE), b'c', False)
        (root / 'Temp' / '002.part').write_bytes(b'')
        (root / 'Temp' / '002.part.met').write_bytes(
            part_met(EMPTY_HASH, 'empty.bin', EMPTY_SIZE, [(0, EMPTY_SIZE)]))
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0)
        with running_daemon(binary, root, (ec_port, peer_port), ec_class=EC) as (proc, ec):
            wait_for(proc, 'shares loaded', lambda: {'active.bin', 'kept.bin'} <= set(shared(ec)))
            assert 'empty.bin' not in shared(ec)

            delete(ec, EMPTY_HASH)
            rename(ec, shared_hash, 'renamed.bin')
            delete(ec, shared_hash)
            wait_for(proc, 'downloads removed', lambda: not ec.files())
            assert set(shared(ec)) == {'kept.bin'}, shared(ec)
            text = (root / 'logfile').read_text(errors='replace')
            assert DESYNC not in text, [l for l in text.splitlines() if DESYNC in l]
    print('Download removals left the path index in step')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
