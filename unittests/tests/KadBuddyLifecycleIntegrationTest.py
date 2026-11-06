#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""An open node keeps one buddy: extra ones lose their connection, and late requests are not promoted."""
import os
import select
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import retry_port_collisions, wait_for
from DownloadDisplayRatesIntegrationTest import hello
from KadLanModeIntegrationTest import (KADEMLIA2_HELLO_REQ, LAN_MODE_TIMEOUT, kad2_hello, stat, wait_stat,
                                       write_private_contacts)
from KadReceiverKeyLogIntegrationTest import KADEMLIA_FINDBUDDY_REQ, KADEMLIA_FIREWALLED2_REQ
from KadSourceTagsECTest import running_kad_daemon
from UploadRefillIntegrationTest import send, wait_packet
from UploadSlotScopeIntegrationTest import OP_HELLOANSWER

KADEMLIA_FIREWALLED_ACK_RES = 0x59
KADEMLIA_FINDBUDDY_RES = 0x5A
OP_HELLO = 0x01
BUDDY_DISCONNECTED = 0
BUDDY_CONNECTED = 2
# A hello's little-endian client ID of 7 is a LowID: we drop a buddy with a HighID.
LOW_ID_ADDRESS = '7.0.0.0'
REQUEST_TIMEOUT = 120
IDLE_TIMEOUT = 40
# The client list runs about once a second.
MARGIN = 5


def kad_peer(address, udp_port):
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind((address, 0))
    peer.connect(('127.0.0.1', udp_port))
    peer.settimeout(10)
    return peer


def kad_send(peer, opcode, body=b''):
    peer.send(bytes([0xE4, opcode]) + body)


def wait_kad_packet(peer, opcode):
    while peer.recv(65535)[:2] != bytes([0xE4, opcode]):
        pass


def pass_firewall_check(udp_port):
    """Two peers report us reachable, which the TCP firewall state needs."""
    for address in ('127.0.0.2', '127.0.0.3'):
        with kad_peer(address, udp_port) as peer:
            kad_send(peer, KADEMLIA2_HELLO_REQ, kad2_hello())
            wait_kad_packet(peer, KADEMLIA_FIREWALLED2_REQ)
            kad_send(peer, KADEMLIA_FIREWALLED_ACK_RES)


class Requester:
    """A firewalled node whose buddy request the daemon accepted."""

    def __init__(self, address, udp_port, peer_port):
        self.address = address
        self.peer_port = peer_port
        self.sock = socket.socket()
        self.sock.bind((address, 0))
        self.port = self.sock.getsockname()[1]
        user_id = os.urandom(16)
        # CUInt128 reads four little-endian words, and the hash is their big-endian bytes.
        self.user_hash = struct.pack('>4I', *struct.unpack('<4I', user_id))
        with kad_peer(address, udp_port) as peer:
            kad_send(peer, KADEMLIA_FINDBUDDY_REQ, os.urandom(16) + user_id + struct.pack('<H', self.port))
            wait_kad_packet(peer, KADEMLIA_FINDBUDDY_RES)

    def send_hello(self):
        self.sock.connect(('127.0.0.1', self.peer_port))
        send(self.sock, OP_HELLO, hello(self.address.encode(), LOW_ID_ADDRESS, self.port, user_hash=self.user_hash))

    def wait_hello_answer(self):
        assert wait_packet(self.sock, OP_HELLOANSWER, 30) is not None, 'no hello answer'

    def connect(self):
        self.send_hello()
        self.wait_hello_answer()

    def closed(self):
        """Whether the daemon closed this socket, draining what it has sent."""
        while select.select([self.sock], [], [], 0)[0]:
            try:
                if not self.sock.recv(65536):
                    return True
            except ConnectionError:
                return True
        return False


def check_extra_buddy_is_dropped(proc, ec, udp_port, peer_port):
    first = Requester('127.0.0.4', udp_port, peer_port)
    second = Requester('127.0.0.5', udp_port, peer_port)
    # Hellos in the same cycle normally both become connected buddies, before either can be refused.
    first.send_hello()
    second.send_hello()
    first.wait_hello_answer()
    second.wait_hello_answer()
    wait_stat(proc, ec, 'EC_TAG_STATS_BUDDY_STATUS', BUDDY_CONNECTED, MARGIN)

    wait_for(proc, 'the extra buddy to lose its idle connection',
             lambda: first.closed() or second.closed(), IDLE_TIMEOUT + MARGIN)
    assert not (first.closed() and second.closed()), 'both buddies lost their connection'

    buddy = second if first.closed() else first
    expected_ip = int.from_bytes(socket.inet_aton(buddy.address), 'little')
    assert stat(ec, 'EC_TAG_STATS_BUDDY_IP') == expected_ip, 'the buddy is not the connection that stayed'
    assert stat(ec, 'EC_TAG_STATS_BUDDY_STATUS') == BUDDY_CONNECTED

    first.sock.close()
    second.sock.close()
    wait_stat(proc, ec, 'EC_TAG_STATS_BUDDY_STATUS', BUDDY_DISCONNECTED, MARGIN)


def check_late_request_is_not_promoted(ec, udp_port, peer_port):
    late = Requester('127.0.0.6', udp_port, peer_port)
    time.sleep(REQUEST_TIMEOUT + MARGIN)
    late.connect()
    time.sleep(MARGIN)
    assert stat(ec, 'EC_TAG_STATS_BUDDY_STATUS') == BUDDY_DISCONNECTED, 'an expired request became the buddy'
    late.sock.close()


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-kad-buddy-') as directory:
        root = Path(directory)
        # The UDP firewall check needs a peer on a public IP, and LAN mode skips it. Enough contacts
        # to outlast the test keep LAN mode on.
        write_private_contacts(root, 8)
        with running_kad_daemon(binary, root, FilterLanIPs=0) as (proc, ec, udp_port, peer_port):
            pass_firewall_check(udp_port)
            wait_stat(proc, ec, 'EC_TAG_STATS_KAD_IN_LAN_MODE', 1, LAN_MODE_TIMEOUT)

            check_extra_buddy_is_dropped(proc, ec, udp_port, peer_port)
            check_late_request_is_not_promoted(ec, udp_port, peer_port)
    print('PASS: one buddy kept, the extra one dropped, and an expired request not promoted')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
