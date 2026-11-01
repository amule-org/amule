#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check Kad diagnostics through authenticated EC on an isolated loopback daemon."""
from pathlib import Path
import struct
import socket
import sys
import tempfile
from AllSearchIntegrationTest import C, connect_daemon, free_ports, retry_port_collisions, running_daemon, tag
from DaemonConfig import write_amule_conf


def local_peer():
    # Bind to an address owned by this machine; never send to external fixture IPs.
    try:
        addresses = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM)
    except OSError:
        return None
    for address in addresses:
        ip = address[4][0]
        octets = tuple(map(int, ip.split('.')))
        if octets[0] in (10, 127) or octets[:2] == (192, 168) or (octets[0] == 172 and 16 <= octets[1] <= 31):
            continue
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            peer.bind((ip, 0))
            return peer
        except OSError:
            peer.close()
    return None


def run(binary, populated=False):
    peer = local_peer() if populated else None
    if populated and peer is None:
        print('No owned non-loopback IPv4 address available')
        return 77
    with tempfile.TemporaryDirectory(prefix='amule-kad-diagnostics-') as directory:
        root = Path(directory)
        port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        if peer:
            ip, udp = peer.getsockname()
            contact = struct.pack('<4IIHHB2IB', 0xA0000000, 0, 0, 1,
                                  int.from_bytes(socket.inet_aton(ip), 'big'), udp, udp, 8, 0, 0, 1)
            (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 1) + contact)
        write_amule_conf(root, port, Port=peer_port, UDPPort=udp_port, ConnectToKad=1,
                         Autoconnect=0, FilterLanIPs=0)
        try:
            with running_daemon(binary, root, (port, peer_port, udp_port), args=(),
                                console='stdout.log') as (proc, ec):
                assert C['EC_TAG_STATS_KAD_DISTRIBUTION'] not in ec.call(C['EC_OP_STAT_REQ'])[1]

                def distribution(running=False):
                    op, tags = ec.call(C['EC_OP_STAT_REQ'], [tag(C['EC_TAG_STATS_KAD_DISTRIBUTION'])])
                    assert op == C['EC_OP_STATS'], op
                    wire = tags[C['EC_TAG_STATS_KAD_DISTRIBUTION']][0]
                    assert len(wire) >= 12 and wire[0] == 2
                    flags, local_id, subnets, entries = struct.unpack('!BIIH', wire[1:12])
                    assert flags == int(running) and len(wire) == 12 + entries * 10
                    bins, verified = [0] * 4096, [0] * 4096
                    for offset in range(12, len(wire), 10):
                        index, count, checked = struct.unpack('!HII', wire[offset:offset + 10])
                        assert index < 4096 and count and checked <= count
                        bins[index], verified[index] = count, checked
                    return bins, verified, subnets
                assert sum(distribution()[0]) == 0
                reply = ec.call(C['EC_OP_KAD_START'])
                assert reply[0] == C['EC_OP_NOOP'], reply
                bins, verified, subnets = distribution(True)
                expected = 1 if peer else 0
                assert sum(bins) == expected, bins
                assert sum(verified) == expected, verified
                assert subnets == expected, subnets
                if peer:
                    assert bins[2560] == 1 and verified[2560] == 1, (bins, verified)
                # A fresh authenticated client must receive the same requested snapshot.
                ec.sock.close()
                ec = connect_daemon(proc, port, root, peer_port, udp_port)
                assert distribution(True) == (bins, verified, subnets)
                assert C['EC_TAG_STATS_KAD_DISTRIBUTION'] not in ec.call(C['EC_OP_STAT_REQ'])[1]
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                assert sum(distribution()[0]) == 0
                ec.sock.close()
        finally:
            if peer:
                peer.close()


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, str(Path(sys.argv[1]).resolve()),
                                   '--populated' in sys.argv) or 0)
