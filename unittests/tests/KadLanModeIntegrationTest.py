#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Private Kad contacts reach the routing table, and LAN mode starts, only with the LAN filter off."""
import os
import socket
import struct
import sys
import tempfile
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, wait_for
from KadSourceTagsECTest import running_kad_daemon

KADEMLIA2_HELLO_REQ = 0x11
KADEMLIA2_HELLO_RES = 0x19
KADEMLIA2_REQ = 0x21
KADEMLIA2_RES = 0x29
PRIVATE_CONTACT = int.from_bytes(socket.inet_aton('10.255.0.1'), 'big')
# IsRunningInLANMode() looks at the routing table every 10 seconds.
LAN_MODE_TIMEOUT = 15


def write_private_contacts(root, count=1, *, address=None, udp_port=4672, tcp_port=4662, version=8):
    """Write private nodes.dat contacts; defaults are unreachable and age out normally."""
    first_ip = PRIVATE_CONTACT if address is None else int.from_bytes(socket.inet_aton(address), 'big')
    contacts = b''.join(
        struct.pack('<4IIHHB2IB', 0xA0000000 + index, 0, 0, 1, first_ip + index, udp_port, tcp_port, version, 0, 0, 1)
        for index in range(count))
    (Path(root) / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, count) + contacts)


def kad2_hello(*, peer_id=None, tcp_port=1, version=8):
    """A Kad2 hello body, with a random Kad ID by default and no tags."""
    return (os.urandom(16) if peer_id is None else peer_id) + struct.pack('<HBB', tcp_port, version, 0)


def kad_stats(ec):
    return ec.call(C['EC_OP_STAT_REQ'])[1]


def stat(ec, name):
    return kad_stats(ec)[C[name]][0]


def wait_stat(proc, ec, name, expected, timeout):
    wait_for(proc, f'{name} to become {expected}', lambda: stat(ec, name) == expected, timeout)


def run_daemon(binary, filter_lan):
    with tempfile.TemporaryDirectory(prefix='amule-kad-lan-mode-') as directory:
        root = Path(directory)
        write_private_contacts(root)
        with running_kad_daemon(binary, root, FilterLanIPs=int(filter_lan)) as (proc, ec, udp_port, _):
            # The Kad stats need a first packet. A loopback sender never becomes a contact.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                peer.sendto(bytes([0xE4, KADEMLIA2_HELLO_REQ]) + kad2_hello(), ('127.0.0.1', udp_port))
            wait_for(proc, 'Kad to see the first packet',
                     lambda: C['EC_TAG_STATS_KAD_IN_LAN_MODE'] in kad_stats(ec), LAN_MODE_TIMEOUT)
            if not filter_lan:
                wait_stat(proc, ec, 'EC_TAG_STATS_KAD_IN_LAN_MODE', 1, LAN_MODE_TIMEOUT)
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
