#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Move files into, out of and within a watched shared dir, against an isolated daemon.

inotify reports a move from or to an unwatched dir as a lone IN_MOVED_TO or IN_MOVED_FROM. A
completed download takes that path from Temp into Incoming, after it is already shared under the
Incoming path.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, connect_daemon, exact, free_port, stop_daemon


def tag_list(data, offset, count):
    """Like parse_tags, but keeps repeated tag names: one EC_TAG_KNOWNFILE per shared file."""
    result = []
    for _ in range(count):
        name, kind, length = struct.unpack_from('!HBI', data, offset)
        offset += 7
        children = []
        if name & 1:
            n = struct.unpack_from('!H', data, offset)[0]
            offset += 2
            start = offset
            children, offset = tag_list(data, offset, n)
            length -= offset - start
        result.append((name >> 1, data[offset:offset + length], children))
        offset += length
    return result, offset


def shared(ec):
    """Map each shared file name to its eD2k hash."""
    payload = bytes([C['EC_OP_GET_SHARED_FILES']]) + struct.pack('!H', 0)
    ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    _, length = struct.unpack('!II', exact(ec.sock, 8))
    reply = exact(ec.sock, length)
    assert reply[0] == C['EC_OP_SHARED_FILES'], reply[0]
    files = {}
    for name, _, children in tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]:
        if name == C['EC_TAG_KNOWNFILE']:
            fields = {child: value for child, value, _ in children}
            files[fields[C['EC_TAG_PARTFILE_NAME']].rstrip(b'\0').decode()] = fields[C['EC_TAG_PARTFILE_HASH']]
    return files


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
    port = free_port()
    (root / 'amule.conf').write_text(f'''[eMule]
Nick=watcher-regression
FirstRunWizardDone=1
Port={free_port()}
UDPEnable=0
Address=127.0.0.1
ConnectToKad=0
ConnectToED2K=0
NewVersionCheck=0
GeoIPEnabled=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
TempDir={root}/Temp
IncomingDir={incoming}
AutoRescanSharedDirs=1
# The checks match English log lines; LC_ALL alone does not override a translated LANG.
Language=en_US
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
''')
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
    env.pop('LD_PRELOAD', None)
    with (root / 'console.log').open('w') as console:
        proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], stdout=console, stderr=console, env=env)
        ec = None
        try:
            ec = connect_daemon(proc, port)
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
        finally:
            if ec:
                ec.sock.close()
            stop_daemon(proc)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='amule-share-watcher-') as scratch:
        # Resolved: macOS FSEvents reports /private/var for /var.
        run(binary, Path(scratch).resolve())
    print('Shared-dir watcher move checks passed')


if __name__ == '__main__':
    main()
