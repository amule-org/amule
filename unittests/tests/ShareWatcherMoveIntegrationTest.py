#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Move files into, out of and within a watched shared dir, against an isolated daemon.

inotify reports a move from or to an unwatched dir as a lone IN_MOVED_TO or IN_MOVED_FROM. A
completed download takes that path from Temp into Incoming, after it is already shared under the
Incoming path.
"""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

import AllSearchIntegrationTest
from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf


def shared(ec):
    """Map each shared file name to its eD2k hash."""
    opcode, tags = ec.call_list(C['EC_OP_GET_SHARED_FILES'])
    assert opcode == C['EC_OP_SHARED_FILES'], opcode
    files = {}
    for name, _, children in tags:
        if name == C['EC_TAG_KNOWNFILE']:
            fields = {child: value for child, value, _ in children}
            files[fields[C['EC_TAG_PARTFILE_NAME']].rstrip(b'\0').decode()] = fields[C['EC_TAG_PARTFILE_HASH']]
    return files


def wait_shared(proc, ec, names, timeout):
    """shared() once it lists every file in names."""
    def listed():
        files = shared(ec)
        return files if set(names) <= files.keys() else None
    return AllSearchIntegrationTest.wait_for(proc, f'{", ".join(names)} to be shared', listed,
                                             timeout)


def log(ec):
    op, tags = ec.call(C['EC_OP_GET_LOG'])
    assert op == C['EC_OP_LOG'], op
    return tags[C['EC_TAG_STRING']][0].decode(errors='replace')


def wait_for(proc, what, check):
    # The watcher waits 5 s for events to settle before it acts. No deadline here: ctest or the
    # caller owns it, as in the other integration tests.
    while not check():
        if proc.poll() is not None:
            raise AssertionError(f'daemon exited while waiting for {what}')
        time.sleep(0.2)


def run(binary, root):
    incoming, outside = root / 'Incoming', root / 'outside'
    for d in (incoming, outside, root / 'Temp'):
        d.mkdir()
    (incoming / 'a.bin').write_bytes(os.urandom(300_000))
    (incoming / 'c.bin').write_bytes(os.urandom(150_000))
    port, peer_port = free_ports(2)
    write_amule_conf(root, port, Port=peer_port, UDPEnable=0, IncomingDir=incoming,
                     AutoRescanSharedDirs=1)
    with running_daemon(binary, root, (port, peer_port)) as (proc, ec):
        wait_for(proc, 'a.bin and c.bin shared', lambda: {'a.bin', 'c.bin'} <= shared(ec).keys())
        original, c_hash = shared(ec)['a.bin'], shared(ec)['c.bin']

        # The completion shape: a byte-identical file with the same mtime arrives over the
        # shared path. b.bin moves in alongside it; once b.bin is being hashed, the watcher has
        # handled both moves.
        shutil.copy2(incoming / 'a.bin', outside / 'a.bin')
        (outside / 'b.bin').write_bytes(os.urandom(200_000))
        os.replace(outside / 'a.bin', incoming / 'a.bin')
        os.replace(outside / 'b.bin', incoming / 'b.bin')
        wait_for(proc, 'b.bin hashing', lambda: f'Hashing file: {incoming / "b.bin"}' in log(ec))
        lines = [l for l in log(ec).splitlines() if 'a.bin' in l]
        assert f'Stopped sharing removed file: {incoming / "a.bin"}' not in '\n'.join(lines), lines
        assert shared(ec).get('a.bin') == original
        wait_for(proc, 'b.bin shared', lambda: 'b.bin' in shared(ec))

        # Moved out of the watched set: no longer shared.
        os.replace(incoming / 'b.bin', outside / 'b.bin')
        wait_for(proc, 'b.bin unshared', lambda: 'b.bin' not in shared(ec))
        assert f'Stopped sharing removed file: {incoming / "b.bin"}' in log(ec)

        # Moved in over a shared file with different content: re-hashed.
        (outside / 'a.bin').write_bytes(os.urandom(250_000))
        os.replace(outside / 'a.bin', incoming / 'a.bin')
        wait_for(proc, 'a.bin re-hashed', lambda: shared(ec).get('a.bin') not in (None, original))

        # Renamed over another shared file: that path now shares the renamed file's content.
        os.replace(incoming / 'c.bin', incoming / 'a.bin')
        wait_for(proc, 'c.bin content shared as a.bin',
                 lambda: shared(ec).get('a.bin') == c_hash and 'c.bin' not in shared(ec))

        # Deleted and written again within one watcher window: shared with the new content.
        os.remove(incoming / 'a.bin')
        (incoming / 'a.bin').write_bytes(os.urandom(120_000))
        wait_for(proc, 'a.bin re-hashed after delete and rewrite',
                 lambda: shared(ec).get('a.bin') not in (None, c_hash))


def run_into_tree(binary, root):
    """A shared file moved from Incoming into a recursive share stays shared.

    The new path is seen while the old one is still shared, so the file looks like a duplicate.
    """
    incoming, tv = root / 'Incoming', root / 'tv'
    for d in (incoming, tv / 'show', root / 'Temp'):
        d.mkdir(parents=True)
    names = [f'm{i}.bin' for i in range(8)]
    for name in names + ['new.bin', 'renamed.bin']:
        (incoming / name).write_bytes(os.urandom(100_000))
    (root / 'shareddir-recursive.dat').write_text(f'{tv}\n')
    port, peer_port = free_ports(2)
    write_amule_conf(root, port, Port=peer_port, UDPEnable=0, IncomingDir=incoming,
                     AutoRescanSharedDirs=1)
    with running_daemon(binary, root, (port, peer_port)) as (proc, ec):
        before = wait_shared(proc, ec, names + ['new.bin', 'renamed.bin'], 60)

        # Into a folder made just before: the move lands before the folder is watched.
        (tv / 'new').mkdir()
        os.replace(incoming / 'new.bin', tv / 'new' / 'new.bin')
        # Copy and delete, as a move across filesystems does. Eight files, because the old and
        # new path are applied in no fixed order.
        for name in names:
            shutil.copy2(incoming / name, tv / 'show' / name)
            os.remove(incoming / name)
        os.replace(incoming / 'renamed.bin', tv / 'show' / 'renamed.bin')
        # Written in the same watcher window, so once it is shared that window has been applied.
        (incoming / 'barrier.bin').write_bytes(os.urandom(50_000))
        wait_shared(proc, ec, ['barrier.bin'], 60)
        time.sleep(1)

        after = shared(ec)
        lost = sorted(n for n in before if after.get(n) != before[n])
        assert not lost, f'no longer shared after the move: {lost}'


def main():
    binary = str(Path(sys.argv[1]).resolve())
    for test in (run, run_into_tree):
        def attempt(test=test):
            with tempfile.TemporaryDirectory(prefix='amule-share-watcher-') as scratch:
                # Resolved: macOS FSEvents reports /private/var for /var.
                test(binary, Path(scratch).resolve())
        retry_port_collisions(attempt)
    print('Shared-dir watcher move checks passed')


if __name__ == '__main__':
    main()
