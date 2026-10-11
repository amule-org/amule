#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise large-index handoff and restart/failure in an isolated real daemon."""
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from AllSearchIntegrationTest import C, connect_daemon, free_port


def indexed_load(ec):
    """Adopted load-entry count, or None before the saved index is adopted."""
    _, tags = ec.call(C['EC_OP_STAT_REQ'])
    return tags.get(C['EC_TAG_STATS_KAD_INDEXED_LOAD'], [None])[0]


def keyword_round_trip(udp_port, key_id, publish):
    """Publish over the real UDP listener and request the stored keyword result."""
    file_id = bytes(12) + struct.pack('<I', 0x12345678)
    name = b'recovered-index-fixture.bin'
    tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            + b'\x03\x01\x00\x02' + struct.pack('<I', 4096)
            + b'\x02\x01\x00\x03\x07\x00Program')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind(('127.0.0.1', 0))
        peer.settimeout(10)
        if publish:
            peer.sendto(b'\xe4\x43' + key_id + struct.pack('<H', 1)
                        + file_id + b'\x03' + tags, ('127.0.0.1', udp_port))
            response, _ = peer.recvfrom(65535)
            assert response[:2] == b'\xe4\x4b', ('publish response', response)
            assert response[2:18] == key_id, 'publication acknowledged the wrong keyword'
        peer.sendto(b'\xe4\x33' + key_id + struct.pack('<H', 0), ('127.0.0.1', udp_port))
        response, _ = peer.recvfrom(65535)
        assert response[:2] == b'\xe4\x3b', ('search response', response)
        assert response[18:34] == key_id, 'search returned the wrong keyword'
        assert struct.unpack_from('<H', response, 34)[0] == 1
        assert response[36:52] == file_id, 'search returned the wrong file'
        assert name in response, 'stored filename was not served'


binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='kad-index-review-') as directory:
    root = Path(directory)
    port = free_port()
    udp_port = free_port()
    count = 50000
    with (root / 'load_index.dat').open('wb') as persisted:
        persisted.write(struct.pack('<III', 1, int(time.time()), count))
        for i in range(1, count + 1):
            persisted.write(bytes(12) + struct.pack('<II', i, int(time.time()) + 3600))
    (root / 'amule.conf').write_text(f"""[eMule]
Nick=review
Port={free_port()}
UDPPort={udp_port}
Address=127.0.0.1
ConnectToKad=1
Autoconnect=0
ConnectToED2K=0
NewVersionCheck=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
TempDir={root}/Temp
IncomingDir={root}/Incoming
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
""")
    with (root / 'stdout.log').open('w') as log:
        env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'xdg'))
        proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
        try:
            ec = connect_daemon(proc, port)
            started = time.monotonic()
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            # The counters are absent until adoption; a hang is left to the ctest timeout.
            while True:
                loaded = indexed_load(ec)
                assert loaded in (None, count), 'partial index escaped the worker'
                if loaded == count:
                    break
                time.sleep(0.05)
            adoption_seconds = time.monotonic() - started
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
            assert (root / 'key_index.dat').exists(), 'empty index was not adopted and saved'
            key_id = (root / 'key_index.dat').read_bytes()[8:24]
            corrupt = struct.pack('<I', 4)
            (root / 'key_index.dat').write_bytes(corrupt)
            # Adoption quarantines the damaged keyword file while retaining the
            # healthy 50,000-entry load map. EC remains responsive across restarts.
            for i in range(10):
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                while indexed_load(ec) != count:
                    time.sleep(0.05)
                assert (root / 'key_index.dat.bad').read_bytes() == corrupt
                # Publish only in the recovery session. Later searches must be
                # served from the saved/reloaded keyword, without re-publication.
                keyword_round_trip(udp_port, key_id, publish=(i == 0))
                _, tags = ec.call(C['EC_OP_STAT_REQ'])
                assert tags[C['EC_TAG_STATS_KAD_INDEXED_KEYWORDS']][0] == 1
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                assert (root / 'key_index.dat').read_bytes() != corrupt
                assert ec.call(C['EC_OP_STAT_REQ'])[0] == C['EC_OP_STATS']
                assert proc.poll() is None
            ec.sock.close()
            print(f'Real daemon: 50000-entry adoption in {adoption_seconds:.3f}s/save and corrupt-index recovery and 10 restart cycles passed; damaged file quarantined and keyword publication/search served with EC responsive.')
        except BaseException:
            log.flush()
            print((root / 'stdout.log').read_text(), file=sys.stderr)
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
