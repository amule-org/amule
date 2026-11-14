#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe real source publication, queued packets and duplicate replies on a private LAN peer."""
import re
import socket
import struct
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path
from AllSearchIntegrationTest import C, retry_port_collisions
from DownloadDisplayRatesIntegrationTest import local_ipv4
from KadLanModeIntegrationTest import (KADEMLIA2_HELLO_REQ, KADEMLIA2_HELLO_RES,
                                      KADEMLIA2_REQ, KADEMLIA2_RES,
                                      kad2_hello, write_private_contacts)
from KadSourceTagsECTest import (KADEMLIA2_PUBLISH_SOURCE_REQ, KADEMLIA2_PUBLISH_RES,
                                running_kad_daemon, unpack_kad_packet)


# One more file than KADEMLIATOTALSTORESRC, so the last lookup waits for a slot.
FILES = 4


def fields(line):
    return {key: int(value) for key, value in re.findall(r'\b(\w+)=(\d+)(?=\s|$)', line)}


def run(binary):
    ip = local_ipv4(private_only=True)
    if ip is None:
        print('No owned private non-loopback IPv4 address for isolated Kad peer')
        return 77
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer, \
            tempfile.TemporaryDirectory(prefix='amule-publish-metrics-') as directory:
        root = Path(directory)
        incoming = root / 'Incoming'
        incoming.mkdir()
        for n in range(FILES):
            (incoming / f'publication{n}.bin').write_bytes(f'publication fixture {n}\n'.encode() * 100)
        peer.bind((ip, 0))
        peer.settimeout(0.1)
        peer_port = peer.getsockname()[1]
        version = 5
        peer_id = struct.pack('<4I', 0xA0000000, 0, 0, 1)
        write_private_contacts(root, address=ip, udp_port=peer_port,
                               tcp_port=peer_port, version=version)
        sections = {'Debug': {'Cat_Kademlia Publish': 1, 'Cat_Kademlia Search': 0}}
        with running_kad_daemon(binary, root, FilterLanIPs=0, VerboseDebug=1,
                                VerboseDebugLogfile=1, sections=sections) as (proc, ec, udp_port, _):
            # An owned private address enables real LAN-mode source publication, without
            # joining the public DHT or bypassing the production publication policy.
            hello = kad2_hello(peer_id=peer_id, tcp_port=peer_port, version=version)
            peer.sendto(bytes([0xE4, KADEMLIA2_HELLO_REQ]) + hello, ('127.0.0.1', udp_port))
            source_packets = 0
            lookup_packets = 0
            targets = set()
            # The first self-lookup starts after 180 seconds, then enables publishing
            # after its ordinary 45-second lifetime. Do not accelerate production timers.
            deadline = time.monotonic() + 360
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'daemon exited during publication'
                try:
                    data, address = peer.recvfrom(65535)
                except socket.timeout:
                    data = b''
                if data and address != ('127.0.0.1', udp_port):
                    continue
                if data:
                    data = unpack_kad_packet(data)
                    if data[:2] == bytes([0xE4, KADEMLIA2_HELLO_REQ]):
                        peer.sendto(bytes([0xE4, KADEMLIA2_HELLO_RES]) + hello, address)
                    elif data[:2] == bytes([0xE4, KADEMLIA2_REQ]):
                        lookup_packets += 1
                        peer.sendto(bytes([0xE4, KADEMLIA2_RES]) + data[3:19] + bytes(1), address)
                    elif data[:2] == bytes([0xE4, KADEMLIA2_PUBLISH_SOURCE_REQ]):
                        source_packets += 1
                        target = data[2:18]
                        targets.add(target)
                        reply = bytes([0xE4, KADEMLIA2_PUBLISH_RES]) + target + bytes(1)
                        peer.sendto(reply, address)
                        peer.sendto(reply, address) # Duplicate acknowledgements count once.
                lines = (root / 'logfile').read_text(errors='replace').splitlines()
                summaries = [fields(line) for line in lines if 'Kad source publish totals:' in line]
                if summaries and summaries[-1]['finished'] == FILES:
                    summary = summaries[-1]
                    assert not any('[Kademlia Search]' in line for line in lines), 'lookup-step logging enabled'
                    assert all('[Kademlia Publish]' in line for line in lines
                               if 'Kad source publish' in line), 'publication used the wrong category'
                    assert source_packets == len(targets) == FILES, (source_packets, len(targets))
                    assert summary['queued'] == FILES, summary
                    assert summary['acknowledged_ips'] == FILES, summary
                    assert summary['attempts'] == summary['starts'] == FILES, summary
                    assert summary['start_failures'] == 0, summary
                    assert summary['zero_ack'] == summary['cancelled'] == 0, summary
                    assert summary['max_req_per_ip_60s'] > 0 and lookup_packets > 0, summary
                    assert summary['max_pub_per_ip_60s'] == FILES, summary
                    # JumpStart exhausts each single-contact lookup and stops it; it then
                    # lingers 15 s for late replies. A stopping lookup frees its slot, so
                    # the last one starts a few 2 s publish ticks after the first.
                    starts = [datetime.fromisoformat(line.split()[0]) for line in lines
                              if 'Kad source publish attempt:' in line and 'started=1' in line]
                    gap = (starts[-1] - starts[0]).total_seconds()
                    assert gap < 13, f'the last lookup waited {gap:.0f} s for a slot'
                    break
            else:
                raise AssertionError('No expected Kad2 publication result:\n' + '\n'.join(lines[-100:]))
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
    print('PASS: Kad2 queued publication, duplicate acknowledgements and totals')


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, str(Path(sys.argv[1]).resolve())) or 0)
