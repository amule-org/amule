#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Kad source entry decides whether the buddy route looks the buddy up, or stays unused.

With TAG_BUDDYHASH and no buddy address, the downloader looks the buddy ID up and asks the nodes
around it to relay a callback. Without the tag there is no buddy ID at all: the route must stay
unused, not look up the zero Kad ID. Records in that shape stay in the network for a republish
period after a publisher stops making them, so a receiver still has to handle them.

One fake Kad node is the daemon's only contact. It answers the bootstrap, the file lookup and the
source search, then records the lookups and callback requests the daemon sends next.
"""
import socket
import struct
import sys
import tempfile
import threading
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadLanModeIntegrationTest import (KADEMLIA2_HELLO_REQ, KADEMLIA2_HELLO_RES, KADEMLIA2_REQ,
                                       KADEMLIA2_RES, kad2_hello, write_private_contacts)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_RES, KADEMLIA2_SEARCH_SOURCE_REQ, TAG_BUDDYHASH,
                                 TAG_FILESIZE, TAG_SOURCEPORT, TAG_SOURCETYPE, running_kad_daemon,
                                 tag as kad_tag, unpack_kad_packet)

KADEMLIA2_BOOTSTRAP_REQ, KADEMLIA2_BOOTSTRAP_RES = 0x01, 0x09
KADEMLIA2_PING, KADEMLIA2_PONG = 0x60, 0x61
KADEMLIA_CALLBACK_REQ = 0x52
TAG_SOURCEIP = b'\xfe'
ZERO_ID = bytes(16)
# The fake node's Kad ID, as write_private_contacts packs the first contact.
NODE_ID = struct.pack('<4I', 0xA0000000, 0, 0, 1)
# Below 6, so the daemon sends this contact plain packets instead of obfuscated ones.
NODE_VERSION = 5
BUDDY_HASH = b'5b' * 16
FILE_HASH = '0123456789abcdef0123456789abcdef'
FILE_SIZE = 9728000
SOURCE_ID = b'\x5a' * 16
# TEST-NET-2, which IsGoodIP() accepts, so the source is not filtered away.
SOURCE_ADDRESS = '198.51.100.7'
SOURCE_PORT = 4662
SETTLE_SECONDS = 25


def private_address():
    """A private, non-loopback IPv4 of this host. Kad never accepts a loopback contact."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(('10.255.255.255', 1))  # no packet leaves the host
            address = probe.getsockname()[0]
        except OSError:
            return None
    octets = address.split('.')
    if octets[0] == '10' or (octets[0] == '192' and octets[1] == '168') or (
            octets[0] == '172' and 16 <= int(octets[1]) <= 31):
        return address
    return None


def kad_ip(address):
    """An IPv4 address the way Kad carries it: network-order bytes read little-endian."""
    return int.from_bytes(socket.inet_aton(address), 'little')


def source_tags(with_buddy_hash, source_type=3):
    """A firewalled source with no buddy address, as older publishers sent it."""
    tags = [kad_tag(TAG_SOURCETYPE, 0x09, source_type),
            kad_tag(TAG_SOURCEIP, 0x03, kad_ip(SOURCE_ADDRESS)),
            kad_tag(TAG_SOURCEPORT, 0x08, SOURCE_PORT),
            kad_tag(TAG_FILESIZE, 0x03, FILE_SIZE)]
    if source_type == 7:
        # A route the receiver parses and then refuses: 2001:db8::/32 is documentation space, so
        # the record reaches the type 7 branch of the downloader and nothing is ever dialled.
        tags.append(kad_tag(b'ip6', 0x02, b'20010db8' * 4))
    if with_buddy_hash:
        tags.append(kad_tag(TAG_BUDDYHASH, 0x02, BUDDY_HASH))
    return tags


class FakeNode(threading.Thread):
    """The daemon's only Kad contact."""

    daemon = True

    def __init__(self, address, with_buddy_hash, source_type=3):
        super().__init__()
        self.with_buddy_hash = with_buddy_hash
        self.source_type = source_type
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((address, 0))
        self.sock.settimeout(0.5)
        self.port = self.sock.getsockname()[1]
        self.lookups = []
        self.callbacks = []
        self.source_searches = 0
        self.running = True

    def send(self, peer, opcode, body=b''):
        self.sock.sendto(bytes([0xE4, opcode]) + body, peer)

    def run(self):
        while self.running:
            try:
                packet, peer = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            packet = unpack_kad_packet(packet)
            if packet[:1] == b'\xe4':
                self.handle(packet[1], packet[2:], peer)

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_BOOTSTRAP_REQ:
            self.send(peer, KADEMLIA2_BOOTSTRAP_RES,
                      NODE_ID + struct.pack('<HBH', SOURCE_PORT, NODE_VERSION, 0))
        elif opcode == KADEMLIA2_HELLO_REQ:
            self.send(peer, KADEMLIA2_HELLO_RES,
                      kad2_hello(peer_id=NODE_ID, tcp_port=SOURCE_PORT, version=NODE_VERSION))
        elif opcode == KADEMLIA2_PING:
            self.send(peer, KADEMLIA2_PONG)
        elif opcode == KADEMLIA2_REQ:
            # <type 1><target 16><receiver 16>. No contacts, so the lookup converges on us
            # and the follow-up request comes here.
            target = body[1:17]
            self.lookups.append(target)
            self.send(peer, KADEMLIA2_RES, target + b'\x00')
        elif opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            # <target 16><startposition 2><filesize 8>
            self.source_searches += 1
            tags = source_tags(self.with_buddy_hash, self.source_type)
            self.send(peer, KADEMLIA2_SEARCH_RES,
                      NODE_ID + body[:16] + struct.pack('<H', 1) + SOURCE_ID
                      + bytes([len(tags)]) + b''.join(tags))
        elif opcode == KADEMLIA_CALLBACK_REQ:
            self.callbacks.append(body)

    def stop(self):
        self.running = False
        self.join(timeout=5)
        self.sock.close()


def drive(binary, address, with_buddy_hash, source_type=3):
    """Feed one daemon a source entry and return the fake node it talked to."""
    node = FakeNode(address, with_buddy_hash, source_type)
    node.start()
    try:
        with tempfile.TemporaryDirectory(prefix='kad-source-buddy-') as directory:
            root = Path(directory)
            write_private_contacts(root, 1, address=address, udp_port=node.port,
                                   tcp_port=SOURCE_PORT, version=NODE_VERSION)
            # One private contact puts Kad in LAN mode, where IsFirewalled() is false, so the
            # buddy route is eligible without staging a firewall check.
            with running_kad_daemon(binary, root, FilterLanIPs=0) as (proc, ec, _udp, _peer):
                wait_for(proc, 'the fake node to be asked for contacts',
                         lambda: len(node.lookups) > 0, 60)
                link = f'ed2k://|file|buddy-id.bin|{FILE_SIZE}|{FILE_HASH}|/'
                assert ec.call(C['EC_OP_ADD_LINK'],
                               [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                wait_for(proc, 'the daemon to search Kad for sources of the file',
                         lambda: node.source_searches > 0, 180)
                if with_buddy_hash and source_type != 7:
                    wait_for(proc, 'a callback request for the published buddy ID',
                             lambda: bool(node.callbacks), 120)
                else:
                    _settle(proc, node)
    finally:
        node.stop()
    return node


def _settle(proc, node, seconds=SETTLE_SECONDS):
    """Give the daemon time to contact the source. Buddy traffic now is the defect, so the
    timeout is what passes."""
    try:
        wait_for(proc, 'buddy traffic that must not happen',
                 lambda: ZERO_ID in node.lookups or bool(node.callbacks), seconds)
    except AssertionError:
        return
    raise AssertionError(f'buddy traffic without a buddy hash: '
                         f'lookups={[target.hex() for target in node.lookups]} '
                         f'callbacks={[body.hex() for body in node.callbacks]}')


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    published = drive(binary, address, with_buddy_hash=True)
    assert published.callbacks, 'no callback request for a source that published a buddy hash'
    for body in published.callbacks:
        assert body[:16] != ZERO_ID, ('callback request for the zero buddy ID', body.hex())
    assert any(target != ZERO_ID for target in published.lookups), published.lookups

    silent = drive(binary, address, with_buddy_hash=False)
    assert silent.source_searches > 0, 'the daemon never searched Kad for sources'
    assert ZERO_ID not in silent.lookups, 'looked up the zero Kad ID'
    assert not silent.callbacks, ('callback request without a buddy hash',
                                  [body.hex() for body in silent.callbacks])
    # Type 7 names its routes in tags, so its buddy hash must start no callback whatever it says.
    routes_in_tags = drive(binary, address, with_buddy_hash=True, source_type=7)
    assert routes_in_tags.source_searches > 0, 'the daemon never searched Kad for sources'
    assert not routes_in_tags.callbacks, ('callback request for a type 7 source',
                                          [body.hex() for body in routes_in_tags.callbacks])
    print('PASS: a published buddy hash is looked up; one without it, and a type 7, get no buddy '
          'lookup')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
