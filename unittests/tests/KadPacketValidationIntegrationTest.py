#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Malformed counted Kad records must not publish a valid prefix to the index."""
from pathlib import Path
import socket
import struct
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from KadSourceTagsECTest import wait_ready


def entry(number):
    name = f'packet-{number}.bin'.encode()
    return (bytes(12) + struct.pack('<I', number) + b'\x02'
            + b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            + b'\x03\x01\x00\x02' + struct.pack('<I', 4096))


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-kad-packet-validation-') as directory:
        root = Path(directory)
        ec_port, udp_port, peer_port = free_ports(3, udp_indices=(1,), peer_index=2)
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToKad=1,
                         Autoconnect=0)
        with running_daemon(binary, root, (ec_port, peer_port, udp_port), args=(),
                            console='stdout.log') as (proc, ec):
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            wait_ready(ec, proc)
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
            key = (root / 'key_index.dat').read_bytes()[8:24]
            assert len(key) == 16
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            wait_ready(ec, proc)

            def indexed():
                assert proc.poll() is None, 'malformed packet killed the daemon'
                return ec.call(C['EC_OP_STAT_REQ'])[1][C['EC_TAG_STATS_KAD_INDEXED_KEYWORDS']][0]

            assert indexed() == 0

            def restart():
                # Reset per-peer flood accounting between malformed cases.
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                wait_ready(ec, proc)

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
                restart()
                publish(2, entry(2) + bytes(15), False)
                assert indexed() == 0, 'truncated ID published its valid prefix'
                restart()
                # The 17-byte minimum fits, but the tag count has no following tag.
                publish(2, entry(3) + bytes(16) + b'\x01', False)
                assert indexed() == 0, 'truncated tag list published its valid prefix'
                restart()
                # A string tag whose declared length exceeds its available bytes.
                publish(2, entry(4) + bytes(16) + b'\x01\x02\x01\x00\x01\xff\xffx', False)
                assert indexed() == 0, 'truncated string published its valid prefix'
                restart()
                publish(0, b'', True)
                assert indexed() == 0
                publish(2, entry(5) + entry(6), True)
                assert indexed() == 2, 'valid multi-record publication changed'
            print('PASS: malformed counted/tagged records leave the index unchanged; '
                  'zero-count and valid multi-record packets are accepted')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
