#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""shareddir-explicit.dat and shareddir-recursive.dat are the only shared-folder inputs.

A legacy shareddir.dat (older aMule, eMule) is migrated once and removed. One that turns up
later is ignored with a warning and removed. Hand edits to the two files stick across
restarts, and a client that sends only the union over EC still changes the explicit list.
"""
import os
from pathlib import Path
import struct
import sys
import tempfile
import time

from AllSearchIntegrationTest import (C, free_ports, retry_port_collisions, running_daemon, string,
                                      tag, wait_for)
from CompletedFileDeleteIntegrationTest import EC
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared

FOLDERS = {'A': 'a.bin', 'B': 'b.bin', 'R': 'r.bin', 'R/sub': 's.bin', 'R2': 'r2.bin'}


def lines(path):
    return path.read_text().splitlines() if path.exists() else []


def write_lines(path, dirs):
    path.write_text(''.join(f'{d}\n' for d in dirs))


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-shareddir-') as scratch:
        root = Path(scratch).resolve()
        (root / 'Incoming').mkdir()
        share = root / 'share'
        folder = {}
        for name, file_name in FOLDERS.items():
            folder[name] = share / name
            folder[name].mkdir(parents=True)
            (folder[name] / file_name).write_bytes(file_name.encode() * 64)
        (root / 'Incoming' / 'barrier.bin').write_bytes(b'barrier' * 64)
        explicit = root / 'shareddir-explicit.dat'
        recursive = root / 'shareddir-recursive.dat'
        legacy = root / 'shareddir.dat'
        (root / 'allowed-remote-dirs.dat').write_text(f'{share}\n')
        ports = free_ports(2, peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPEnable=0, AutoRescanSharedDirs=0)

        def expect_shared(what, names, then=None):
            want = set(names) | {'barrier.bin'}
            with running_daemon(binary, root, ports, ec_class=EC) as (proc, ec):
                wait_for(proc, what, lambda: set(shared(ec)) == want)
                # A late hash would show up here.
                time.sleep(1)
                assert set(shared(ec)) == want, (what, sorted(shared(ec)))
                if then:
                    then(ec)
            # amuled starts a new logfile on every run.
            return (root / 'logfile').read_text(errors='replace')

        # An older aMule listed each subfolder of a recursive share on its own line.
        write_lines(legacy, [folder['A'], folder['R'], folder['R/sub']])
        log = expect_shared('2.3.3-style migration', ['a.bin', 'r.bin', 's.bin'])
        assert 'Moved the shared folders from' in log, log
        assert not legacy.exists(), 'migrated shareddir.dat was kept'
        assert lines(explicit) == [str(folder[n]) for n in ('A', 'R', 'R/sub')], lines(explicit)
        assert lines(recursive) == [], lines(recursive)

        write_lines(explicit, [folder['B'], folder['R'], folder['R/sub']])
        expect_shared('hand edit of shareddir-explicit.dat', ['b.bin', 'r.bin', 's.bin'])
        assert lines(explicit) == [str(folder[n]) for n in ('B', 'R', 'R/sub')], lines(explicit)

        write_lines(explicit, [folder['B']])
        write_lines(recursive, [folder['R']])
        expect_shared('recursive root added by hand', ['b.bin', 'r.bin', 's.bin'])
        write_lines(recursive, [folder['R2']])
        expect_shared('recursive root replaced by hand', ['b.bin', 'r2.bin'])
        assert lines(explicit) == [str(folder['B'])], lines(explicit)

        # An older aMule or a script wrote the union again; A is listed only there.
        write_lines(legacy, [folder['B'], folder['A'], folder['R2']])
        log = expect_shared('legacy file after migration', ['b.bin', 'r2.bin'])
        assert 'Ignoring ' in log and 'The file will be removed.' in log, log
        assert '1 folder listed only there is not shared:' in log and str(folder['A']) in log, log
        assert not legacy.exists(), 'ignored shareddir.dat was kept'
        assert lines(explicit) == [str(folder['B'])], lines(explicit)

        # aMule 3.0 to 3.2 wrote the union of the two files: nothing is lost, so no warning.
        write_lines(legacy, [folder['B'], folder['R2']])
        log = expect_shared('union left by aMule 3.2', ['b.bin', 'r2.bin'])
        assert 'Removing ' in log and 'Ignoring ' not in log, log
        assert not legacy.exists(), 'union shareddir.dat was kept'
        # The same from eMule, whose folders end with a separator.
        legacy.write_bytes(b'\xff\xfe' + ''.join(f'{folder[n]}{os.sep}\r\n' for n in ('B', 'R2'))
                           .encode('utf-16-le'))
        log = expect_shared('eMule file after migration', ['b.bin', 'r2.bin'])
        assert 'Removing ' in log and 'Ignoring ' not in log, log

        # A legacy EC client sends the whole union: B, A, and R2 with its tree.
        def set_union(dirs):
            def apply(ec):
                group = tag(C['EC_TAG_PREFS_DIRECTORIES'], children=[
                    tag(C['EC_TAG_DIRECTORIES_SHARED'], struct.pack('!I', len(dirs)), 4,
                        children=[string(C['EC_TAG_STRING'], str(d)) for d in dirs])])
                assert ec.call('EC_OP_SET_PREFERENCES', [group])[0] == C['EC_OP_NOOP']
            return apply

        expect_shared('union edit over EC', ['b.bin', 'r2.bin'],
                      set_union([folder['B'], folder['A'], folder['R2']]))
        assert lines(explicit) == [str(folder['B']), str(folder['A'])], lines(explicit)
        assert lines(recursive) == [str(folder['R2'])], lines(recursive)
        expect_shared('union edit saved', ['a.bin', 'b.bin', 'r2.bin'], set_union([folder['R2']]))
        assert lines(explicit) == [], lines(explicit)
        assert lines(recursive) == [str(folder['R2'])], lines(recursive)

        # eMule: UTF-16LE behind a byte-order mark, CRLF, a separator after each folder.
        explicit.unlink()
        recursive.unlink()
        legacy.write_bytes(b'\xff\xfe' + f'{folder["A"]}{os.sep}\r\n'.encode('utf-16-le'))
        log = expect_shared('eMule migration', ['a.bin'])
        assert 'Moved the shared folders from' in log, log
        assert lines(explicit) == [str(folder['A'])], lines(explicit)
        assert not legacy.exists(), 'migrated eMule shareddir.dat was kept'

        # A file that does not decode (a lone surrogate) is neither imported nor removed.
        explicit.unlink()
        recursive.unlink()
        legacy.write_bytes(b'\xff\xfe\x00\xd8')
        for attempt in ('first', 'second'):
            log = expect_shared(f'undecodable legacy file, {attempt} start', [])
            assert 'Could not read ' in log and 'Moved the shared folders' not in log, log
            assert legacy.exists(), 'undecodable shareddir.dat was removed'
    print('Shared-folder files: migration, hand edits, legacy file and EC union edit passed')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
