#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check Kad diagnostics through authenticated EC on an isolated loopback daemon."""
from pathlib import Path
import struct
import socket
import time
import sys
import tempfile
from AllSearchIntegrationTest import C, connect_daemon, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf


def local_peer():
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
        print('No owned Kad-compatible IPv4 address available')
        return 77
    with tempfile.TemporaryDirectory(prefix='amule-kad-diagnostics-') as directory:
        root = Path(directory)
        port, ed2k_port, kad_port = free_ports(3, udp_indices=(2,), peer_index=1)
        if peer:
            ip, udp = peer.getsockname()
            contact = struct.pack('<4IIHHB2IB', 0xA0000000, 0, 0, 1,
                                  int.from_bytes(socket.inet_aton(ip), 'big'), udp, udp, 8, 0, 0, 1)
            (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 1) + contact)
        write_amule_conf(root, port, Port=ed2k_port, UDPPort=kad_port, ConnectToKad=1,
                         Autoconnect=0, FilterLanIPs=0)
        try:
            with running_daemon(binary, root, (port, ed2k_port, kad_port), args=(),
                                console='stdout.log') as (proc, ec):
                op, tags = ec.call(C['EC_OP_GET_KAD_LOOKUPS'])
                assert op == C['EC_OP_GET_KAD_LOOKUPS']
                assert C['EC_TAG_KAD_LOOKUP'] not in tags, tags
                # The diagnostic request leaves legacy search operations usable.
                assert ec.call(C['EC_OP_SEARCH_PROGRESS'])[0] == C['EC_OP_SEARCH_PROGRESS']
                if peer:
                    assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                    sid = ec.start('lookup lifecycle regression', kind=C['EC_SEARCH_KAD'])
                    def snapshot():
                        return ec.call(C['EC_OP_GET_KAD_LOOKUPS'])[1].get(C['EC_TAG_KAD_LOOKUP'], (None, {}))[1]
                    def peer_record():
                        return snapshot().get(C['EC_TAG_KAD_LOOKUP_PEER'], (None, {}))[1]
                    while not peer_record():
                        if proc.poll() is not None:
                            raise RuntimeError('daemon exited before lookup reached peer')
                        time.sleep(0.1)
                    active = snapshot()
                    peer_data = active[C['EC_TAG_KAD_LOOKUP_PEER']][1]
                    assert peer_data[C['EC_TAG_KAD_LOOKUP_PEER_IP']][0] == int.from_bytes(socket.inet_aton(ip), 'big'), active
                    assert peer_data[C['EC_TAG_KAD_LOOKUP_PEER_PORT']][0] == udp, active
                    assert peer_data[C['EC_TAG_KAD_LOOKUP_PEER_VERSION']][0] == 8, active
                    assert len(peer_data[C['EC_TAG_KAD_LOOKUP_PEER_DISTANCE']][0]) == 16, active
                    assert active[C['EC_TAG_KAD_LOOKUP_ACTIVE']][0] == 1, active
                    # The actual production search must archive its value snapshot on stop.
                    from AllSearchIntegrationTest import integer
                    assert ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])[0] == C['EC_OP_MISC_DATA']
                    assert snapshot()[C['EC_TAG_KAD_LOOKUP_ACTIVE']][0] == 0, snapshot()
                    ec.sock.close()
                    ec = connect_daemon(proc, port, root, ed2k_port, kad_port)
                    assert peer_record()[C['EC_TAG_KAD_LOOKUP_PEER_PORT']][0] == udp, snapshot()
                    assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                ec.sock.close()
        finally:
            if peer:
                peer.close()


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), '--populated' in sys.argv) or 0)
