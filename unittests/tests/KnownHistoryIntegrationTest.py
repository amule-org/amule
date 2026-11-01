#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A file known.met forgets must still show as downloaded in search results.

The 30-day TTL prune drops known.met records of files no longer on disk. Their hashes go to
knownhistory.met, so a file downloaded and then deleted does not look new again a month later.
"""
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import (C, exact, free_ports, integer, retry_port_collisions,
                                      running_daemon, start_daemon, string)
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import tag_list

FORGOTTEN = bytes(range(16))
UNKNOWN = bytes(range(16, 32))
LONG_AGO = 1577836800  # 2020-01-01, well past the TTL
NEW, DOWNLOADED = 0, 1


def known_met(file_hash, name, size):
    """One record, as CKnownFile::WriteToFile lays it out, for a file under one part."""
    name = name.encode()
    tags = (bytes([0x82, 0x01]) + struct.pack('<H', len(name)) + name  # FT_FILENAME
            + bytes([0x83, 0x02]) + struct.pack('<I', size)  # FT_FILESIZE
            + bytes([0x83, 0x28]) + struct.pack('<I', LONG_AGO))  # FT_LASTSEEN
    record = struct.pack('<I', LONG_AGO) + file_hash + struct.pack('<H', 0) + struct.pack('<I', 3) + tags
    return b'\x0e' + struct.pack('<I', 1) + record


def search_result(file_hash, name):
    name = name.encode()
    tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            + b'\x03\x01\x00\x02' + struct.pack('<I', 4096)
            + b'\x03\x01\x00\x15' + struct.pack('<I', 10))
    return file_hash + struct.pack('<IHI', 0, 0, 3) + tags


def fake_server():
    """Logs the daemon in with a high ID and answers every search with both test files."""
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
                        body = (b'\x33' + struct.pack('<I', 2) + search_result(FORGOTTEN, 'forgotten.bin')
                                + search_result(UNKNOWN, 'unknown.bin'))
                    else:
                        continue
                    peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
        except (EOFError, OSError):
            pass

    threading.Thread(target=serve, daemon=True).start()
    return listener.getsockname()[1]


def start(binary, root, ports):
    return start_daemon(binary, root, ports, server_udp=True)


def wait_for(proc, what, check):
    while not check():
        if proc.poll() is not None:
            raise AssertionError(f'daemon exited while waiting for {what}')
        time.sleep(0.2)


def search_statuses(ec, sid):
    payload = bytes([C['EC_OP_SEARCH_RESULTS']]) + struct.pack('!H', 1) + integer(C['EC_TAG_SEARCH_ID'], sid)
    ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    _, length = struct.unpack('!II', exact(ec.sock, 8))
    reply = exact(ec.sock, length)
    statuses = {}
    for name, _, children in tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]:
        if name == C['EC_TAG_SEARCHFILE']:
            fields = {child: value for child, value, _ in children}
            statuses[fields[C['EC_TAG_PARTFILE_HASH']]] = int.from_bytes(fields[C['EC_TAG_PARTFILE_STATUS']], 'big')
    return statuses


def run(binary, root):
    for d in ('Incoming', 'Temp'):
        (root / d).mkdir()
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    ec_port, peer_port, udp_port = ports
    # The fake server is on loopback, which only an unfiltered LAN setting dials.
    write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToED2K=1,
                     FilterLanIPs=0)
    (root / 'known.met').write_bytes(known_met(FORGOTTEN, 'forgotten.bin', 4096))

    # The prune only runs after the first share scan, and the shutdown save runs it.
    with running_daemon(binary, root, ports, server_udp=True) as (proc, ec):
        wait_for(proc, 'the first share scan',
                 lambda: re.search(r'Found \d+ known shared file', (root / 'logfile').read_text(errors='replace')))
    assert FORGOTTEN not in (root / 'known.met').read_bytes(), 'the TTL prune kept the record'
    history = root / 'knownhistory.met'
    assert history.exists(), 'knownhistory.met was not written'
    assert history.read_bytes() == b'\x21' + struct.pack('<I', 1) + FORGOTTEN, history.read_bytes()

    with running_daemon(binary, root, ports, server_udp=True) as (proc, ec):
        server_port = fake_server()
        assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{server_port}')])[0] == C['EC_OP_NOOP']
        assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
        sid = ec.start('regression', kind=C['EC_SEARCH_LOCAL'], wait=True)
        wait_for(proc, 'both search results', lambda: len(search_statuses(ec, sid)) == 2)
        statuses = search_statuses(ec, sid)
        assert statuses[FORGOTTEN] == DOWNLOADED, statuses
        assert statuses[UNKNOWN] == NEW, statuses


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-known-history-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    print('Known-file history checks passed')


if __name__ == '__main__':
    main()
