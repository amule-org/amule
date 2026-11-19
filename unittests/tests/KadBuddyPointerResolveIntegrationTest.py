#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A source whose buddy does not pass on a callback is asked through the buddy its pointer names.

A LowID that marks its records with `bpv` also stores a pointer to its current buddy at the NOT of
its source ID (emule-qt ipv6-spec §5.6.1). Its records name the buddy it had when it published
them, so after a buddy change they lead to a dead one. When the callback socket through that buddy
times out, the downloader looks up the pointer with a source search of size 0 and asks the buddy it
names. A marked record comes from a publisher that writes the buddy ID in one layout only, so the
dead buddy is not asked again in the other one.

hit:  the pointer names a second buddy on the same IP, which gets the next request once eMule's
      flood interval allows, and nothing follows it.
miss: no pointer is found; after the lookup the route has failed and no request follows.

One fake Kad node is the daemon's only contact, the storer of both records and the dead buddy.
"""
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadLanModeIntegrationTest import write_private_contacts
from KadSourceBuddyIdIntegrationTest import (FILE_HASH, FILE_SIZE, KADEMLIA_CALLBACK_REQ, NODE_ID,
                                             NODE_VERSION, SOURCE_ADDRESS, SOURCE_ID, SOURCE_PORT,
                                             FakeNode, kad_ip, private_address)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_RES, KADEMLIA2_SEARCH_SOURCE_REQ,
                                 TAG_BUDDYHASH, TAG_FILESIZE, TAG_SERVERIP, TAG_SERVERPORT,
                                 TAG_SOURCEIP, TAG_SOURCEPORT, TAG_SOURCETYPE, running_kad_daemon,
                                 tag as kad_tag, unpack_kad_packet)

TAG_BUDDYPOINTER = b'bpv'
BUDDY_HASH = b'00112233445566778899aabbccddeeff'
POINTER_KEY = bytes(0xFF ^ b for b in SOURCE_ID)
# The next request to the same buddy IP waits out eMule's flood interval; the socket gives up at 40 s.
FLOOD_INTERVAL = 60
SOCKET_TIMEOUT = 40
# A pointer lookup runs this long.
LOOKUP_SECONDS = 45
# Long enough for a request that must not come: one more socket timeout and the 65 s wait.
SETTLE_SECONDS = 75


def marked_record(buddy_address, buddy_port, with_size=True):
    """A firewalled source reached through the buddy at buddy_address:buddy_port, with `bpv`."""
    tags = [kad_tag(TAG_SOURCETYPE, 0x09, 3),
            kad_tag(TAG_SOURCEIP, 0x03, kad_ip(SOURCE_ADDRESS)),
            kad_tag(TAG_SOURCEPORT, 0x08, SOURCE_PORT),
            kad_tag(TAG_SERVERIP, 0x03, kad_ip(buddy_address)),
            kad_tag(TAG_SERVERPORT, 0x08, buddy_port),
            kad_tag(TAG_BUDDYHASH, 0x02, BUDDY_HASH),
            kad_tag(TAG_BUDDYPOINTER, 0x09, 1)]
    if with_size:
        tags.append(kad_tag(TAG_FILESIZE, 0x03, FILE_SIZE))
    return tags


class LiveBuddy(threading.Thread):
    """The buddy the pointer names. It records the callback requests and passes none on."""

    daemon = True

    def __init__(self, address, requests):
        super().__init__()
        self.requests = requests
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((address, 0))
        self.sock.settimeout(0.5)
        self.port = self.sock.getsockname()[1]
        self.running = True

    def run(self):
        while self.running:
            try:
                packet, _peer = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            packet = unpack_kad_packet(packet)
            if packet[:2] == bytes([0xE4, KADEMLIA_CALLBACK_REQ]):
                self.requests.append((time.monotonic(), 'pointer', packet[2:18]))

    def stop(self):
        self.running = False
        self.join(timeout=5)
        self.sock.close()


class Storer(FakeNode):
    """Holds the file record, naming itself as the buddy, and the pointer, naming `live`."""

    def __init__(self, address, live):
        super().__init__(address, with_buddy_hash=False)
        self.live = live
        self.requests = live.requests if live else []
        self.pointer_searches = []

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            # <target 16><startposition 2><filesize 8>
            address = self.sock.getsockname()[0]
            if body[:16] == POINTER_KEY:
                self.pointer_searches.append((time.monotonic(), struct.unpack('<Q', body[18:26])[0]))
                records = [marked_record(address, self.live.port, with_size=False)] if self.live else []
            else:
                self.source_searches += 1
                records = [marked_record(address, self.port)]
            entries = b''.join(SOURCE_ID + bytes([len(tags)]) + b''.join(tags) for tags in records)
            self.send(peer, KADEMLIA2_SEARCH_RES, NODE_ID + body[:16]
                      + struct.pack('<H', len(records)) + entries)
        elif opcode == KADEMLIA_CALLBACK_REQ:
            self.requests.append((time.monotonic(), 'record', body[:16]))
        else:
            super().handle(opcode, body, peer)


def drive(binary, address, with_pointer):
    """Feed one daemon the source and return the storer, with every callback request."""
    live = LiveBuddy(address, []) if with_pointer else None
    node = Storer(address, live)
    threads = [thread for thread in (node, live) if thread]
    for thread in threads:
        thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='kad-pointer-resolve-') as directory:
            root = Path(directory)
            write_private_contacts(root, 1, address=address, udp_port=node.port,
                                   tcp_port=SOURCE_PORT, version=NODE_VERSION)
            with running_kad_daemon(binary, root, FilterLanIPs=0) as (proc, ec, _udp, _peer):
                wait_for(proc, 'the fake node to be asked for contacts',
                         lambda: len(node.lookups) > 0, 60)
                link = f'ed2k://|file|pointer-resolve.bin|{FILE_SIZE}|{FILE_HASH}|/'
                assert ec.call(C['EC_OP_ADD_LINK'],
                               [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                wait_for(proc, 'a callback request through the buddy the record names',
                         lambda: bool(node.requests), 180)
                first = node.requests[0][0]
                wait_for(proc, 'the pointer lookup', lambda: bool(node.pointer_searches),
                         SOCKET_TIMEOUT + 30)
                print(f'pointer search {node.pointer_searches[0][0] - first:.1f} s after the first '
                      f'request')
                if with_pointer:
                    wait_for(proc, 'a callback request through the buddy the pointer names',
                             lambda: len(node.requests) >= 2, FLOOD_INTERVAL + 30)
                try:
                    wait_for(proc, 'a callback request after the route failed',
                             lambda: len(node.requests) > (2 if with_pointer else 1),
                             SETTLE_SECONDS if with_pointer else LOOKUP_SECONDS + SETTLE_SECONDS)
                except AssertionError:
                    pass
    finally:
        for thread in threads:
            thread.stop()
    return node


def run(binary, case):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    node = drive(binary, address, with_pointer=case == 'hit')
    requests = node.requests
    print('requests:', ', '.join(f'{t - requests[0][0]:.1f} s {buddy} {ident[:4].hex()}'
                                 for t, buddy, ident in requests))
    assert len(node.pointer_searches) == 1, ('one pointer lookup, asked once of the storer',
                                             node.pointer_searches)
    assert node.pointer_searches[0][1] == 0, 'the pointer search names a file size'
    assert node.pointer_searches[0][0] - requests[0][0] >= SOCKET_TIMEOUT - 1, (
        'the pointer was looked up before the callback socket timed out')
    if case == 'miss':
        assert [buddy for _, buddy, _ in requests] == ['record'], (
            'a request after a pointer lookup that found nothing', requests)
        print('PASS: no pointer, no second layout and no further request')
        return

    assert [buddy for _, buddy, _ in requests] == ['record', 'pointer'], (
        'not the dead buddy once and then the pointer buddy once', requests)
    assert requests[1][2] == requests[0][2], 'the pointer buddy was asked in another layout'
    # Both buddies share one IP, and eMule's limit is per IP.
    assert requests[1][0] - requests[0][0] > FLOOD_INTERVAL, (
        f'the buddy IP was asked again {requests[1][0] - requests[0][0]:.1f} s after the first')
    print('PASS: the buddy the pointer names is asked once the flood interval allows, in the same '
          'layout, and nothing follows')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[2] not in ('hit', 'miss'):
        sys.exit(f'usage: {sys.argv[0]} AMULED hit|miss')
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), sys.argv[2])
