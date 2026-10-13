#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A tag the .met writer cannot write must not make the file unreadable.

The writer skips HASH16 and BOOL tags, but the tag count still included them. In
StoredSearches.met the reader then took the next fields for a tag and every stored search was
lost. A .part.met ends with its tags, so it was only reported corrupt at every start.
"""
import hashlib
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions, stop_daemon,
                                      string, tag)
from KnownHistoryIntegrationTest import search_statuses, start, wait_for

RESULT_HASH = bytes(range(16))
PART_HASH = bytes(range(16, 32))
PART_SIZE = 1 << 20
HASH16_TAG = b'\x81\xf0' + bytes(16)  # unknown name 0xF0
BOOL_TAG = b'\x85\xf1\x01'  # unknown name 0xF1


def search_result():
    name = b'unwritable.bin'
    tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            + b'\x03\x01\x00\x02' + struct.pack('<I', 4096)
            + HASH16_TAG + BOOL_TAG)
    return RESULT_HASH + struct.pack('<IHI', 0, 0, 4) + tags


def fake_server():
    """Logs the daemon in with a high ID and answers every search with one result."""
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)

    def serve():
        try:
            with listener.accept()[0] as peer:
                while True:
                    _, length = struct.unpack('<BI', exact(peer, 5))
                    packet = exact(peer, length)
                    if packet[0] == 0x01:  # OP_LOGINREQUEST
                        body = b'\x40' + struct.pack('<II', 0x12345678, 0)
                    elif packet[0] == 0x16:  # OP_SEARCHREQUEST
                        body = b'\x33' + struct.pack('<I', 1) + search_result()
                    else:
                        continue
                    peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
        except (EOFError, OSError):
            pass

    threading.Thread(target=serve, daemon=True).start()
    return listener.getsockname()[1]


def part_met():
    """An active download whose .part.met carries a HASH16 tag."""
    name = b'active.bin'
    tags = [b'\x82\x01' + struct.pack('<H', len(name)) + name,  # FT_FILENAME
            b'\x83\x02' + struct.pack('<I', PART_SIZE),  # FT_FILESIZE
            HASH16_TAG]
    for mark, value in ((0x09, 0), (0x0a, PART_SIZE)):  # FT_GAPSTART, FT_GAPEND
        tags.append(b'\x03\x02\x00' + bytes([mark]) + b'0' + struct.pack('<I', value))
    return (b'\xe0' + struct.pack('<I', 0) + PART_HASH + struct.pack('<H', 0)
            + struct.pack('<I', len(tags)) + b''.join(tags))


def partfile_known(ec):
    op, tags = ec.call(C['EC_OP_PARTFILE_PAUSE'], [tag(C['EC_TAG_PARTFILE'], PART_HASH, 9)])
    assert op in (C['EC_OP_NOOP'], C['EC_OP_FAILED']), (op, tags)
    return op == C['EC_OP_NOOP']


def run(binary, root):
    for d in ('Incoming', 'Temp'):
        (root / d).mkdir()
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    ec_port, peer_port, udp_port = ports
    (root / 'amule.conf').write_text(f'''[eMule]
Nick=unwritable-tag-regression
FirstRunWizardDone=1
Port={peer_port}
UDPPort={udp_port}
Address=127.0.0.1
ConnectToKad=0
ConnectToED2K=1
NewVersionCheck=0
GeoIPEnabled=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
AddServerListFromServer=0
AddServerListFromClient=0
# The fake server is on loopback, which only an unfiltered LAN setting dials.
FilterLanIPs=0
TempDir={root}/Temp
IncomingDir={root}/Incoming
# The checks match English log lines; LC_ALL alone does not override a translated LANG.
Language=en_US
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={ec_port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
''')
    # Empty data file: a non-empty one with a stale date would be rehashed first.
    (root / 'Temp/001.part').write_bytes(b'')
    (root / 'Temp/001.part.met').write_bytes(part_met())

    proc, ec = start(binary, root, ports)
    try:
        server_port = fake_server()
        assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{server_port}')])[0] == C['EC_OP_NOOP']
        assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
        sid = ec.start('regression', kind=C['EC_SEARCH_LOCAL'], wait=True)
        wait_for(proc, 'the search result', lambda: RESULT_HASH in search_statuses(ec, sid))
        # A resume saves the .part.met.
        assert ec.call(C['EC_OP_PARTFILE_RESUME'], [tag(C['EC_TAG_PARTFILE'], PART_HASH, 9)])[0] == C['EC_OP_NOOP']
    finally:
        ec.sock.close()
        stop_daemon(proc)

    proc, ec = start(binary, root, ports)
    try:
        log = (root / 'logfile').read_text(errors='replace')
        assert 'Invalid entry in stored search list' not in log, 'StoredSearches.met was unreadable'
        assert RESULT_HASH in search_statuses(ec, sid), 'the stored search was not restored'
        assert 'is corrupt (wrong tagcount)' not in log, 'the .part.met tag count was wrong'
        assert partfile_known(ec), 'the download was lost'
    finally:
        ec.sock.close()
        stop_daemon(proc)


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-unwritable-tag-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    print('Unwritable-tag checks passed')


if __name__ == '__main__':
    main()
