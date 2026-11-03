#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Private Kad contacts reach the routing table, and LAN mode starts, only with the LAN filter off."""
import os
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from KadSourceTagsECTest import wait_ready

KADEMLIA2_HELLO_REQ = 0x11
PRIVATE_CONTACT = int.from_bytes(socket.inet_aton('10.255.0.1'), 'big')
# IsRunningInLANMode() looks at the routing table every 10 seconds.
LAN_MODE_TIMEOUT = 15


def kad_stats(ec):
    return ec.call(C['EC_OP_STAT_REQ'])[1]


def run_daemon(binary, filter_lan):
    with tempfile.TemporaryDirectory(prefix='amule-kad-lan-mode-') as directory:
        root = Path(directory)
        ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        contact = struct.pack('<4IIHHB2IB', 0xA0000000, 0, 0, 1, PRIVATE_CONTACT, 4672, 4662, 8, 0, 0, 1)
        (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 1) + contact)
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToKad=1, Autoconnect=0,
                         FilterLanIPs=int(filter_lan))
        with running_daemon(binary, root, (ec_port, peer_port, udp_port)) as (proc, ec):
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            wait_ready(ec, proc)
            # The Kad stats need a first packet. A loopback sender never becomes a contact.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.sendto(bytes([0xE4, KADEMLIA2_HELLO_REQ]) + os.urandom(16) + struct.pack('<HBB', 1, 8, 0),
                            ('127.0.0.1', udp_port))
            deadline = time.monotonic() + LAN_MODE_TIMEOUT
            while C['EC_TAG_STATS_KAD_IN_LAN_MODE'] not in kad_stats(ec):
                assert time.monotonic() < deadline, 'Kad never saw the first packet'
                time.sleep(0.1)
            if not filter_lan:
                while not kad_stats(ec)[C['EC_TAG_STATS_KAD_IN_LAN_MODE']][0]:
                    assert time.monotonic() < deadline, 'LAN mode never started with the filter off'
                    time.sleep(0.1)
            stats = kad_stats(ec)
            expected = 0 if filter_lan else 1
            assert stats[C['EC_TAG_STATS_KAD_NODES']][0] == expected, f'contacts with filter_lan={filter_lan}'
            assert stats[C['EC_TAG_STATS_KAD_IN_LAN_MODE']][0] == expected, f'LAN mode with filter_lan={filter_lan}'


def run(binary):
    run_daemon(binary, filter_lan=False)
    run_daemon(binary, filter_lan=True)
    print('PASS: the private contact loads and LAN mode starts only with the LAN filter off')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
