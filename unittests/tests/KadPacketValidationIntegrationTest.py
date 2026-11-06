#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Malformed counted Kad records must not publish a valid prefix to the index."""
from pathlib import Path
import socket
import struct
import sys
import tempfile

from AllSearchIntegrationTest import C, retry_port_collisions
from KadSourceTagsECTest import kad_id, restart_kad, running_kad_daemon


def entry(number):
    name = f'packet-{number}.bin'.encode()
    return (bytes(12) + struct.pack('<I', number) + b'\x02'
            + b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            + b'\x03\x01\x00\x02' + struct.pack('<I', 4096))


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-kad-packet-validation-') as directory:
        root = Path(directory)
        with running_kad_daemon(binary, root, console='stdout.log') as (proc, ec, udp_port, _):
            key = kad_id(ec, proc, root)

            def indexed():
                assert proc.poll() is None, 'malformed packet killed the daemon'
                return ec.call(C['EC_OP_STAT_REQ'])[1][C['EC_TAG_STATS_KAD_INDEXED_KEYWORDS']][0]

            assert indexed() == 0

            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.bind(('127.0.0.1', 0))
                peer.settimeout(1)

                def publish(count, records, accepted):
                    peer.settimeout(10 if accepted else 1)
                    peer.sendto(b'\xe4\x43' + key + struct.pack('<H', count) + records,
                                ('127.0.0.1', udp_port))
                    try:
                        response, _ = peer.recvfrom(65535)
                    except socket.timeout:
                        assert not accepted, 'valid publication was not acknowledged'
                    else:
                        assert accepted, 'malformed publication was acknowledged'
                        assert response[:18] == b'\xe4\x4b' + key, response

                # Impossible count, then a plausible count with a truncated final ID.
                publish(65535, entry(1), False)
                assert indexed() == 0, 'impossible count published its valid prefix'
                # Reset per-peer flood accounting between malformed cases.
                restart_kad(ec, proc)
                publish(2, entry(2) + bytes(15), False)
                assert indexed() == 0, 'truncated ID published its valid prefix'
                restart_kad(ec, proc)
                # The 17-byte minimum fits, but the tag count has no following tag.
                publish(2, entry(3) + bytes(16) + b'\x01', False)
                assert indexed() == 0, 'truncated tag list published its valid prefix'
                restart_kad(ec, proc)
                # A string tag whose declared length exceeds its available bytes.
                publish(2, entry(4) + bytes(16) + b'\x01\x02\x01\x00\x01\xff\xffx', False)
                assert indexed() == 0, 'truncated string published its valid prefix'
                restart_kad(ec, proc)
                publish(0, b'', True)
                assert indexed() == 0
                publish(2, entry(5) + entry(6), True)
                assert indexed() == 2, 'valid multi-record publication changed'
            print('PASS: malformed counted/tagged records leave the index unchanged; '
                  'zero-count and valid multi-record packets are accepted')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
