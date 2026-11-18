#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""An unanswered callback through a source's buddy is retried in the other buddy ID layout.

A source published before #1993 by aMule wrote TAG_BUDDYHASH in another byte order. The ID must
match on the way: emule-qt relays only an ID it serves, and the LowID drops a callback that is not
its own. So when a KADEMLIA_CALLBACK_REQ goes unanswered, the downloader asks the same buddy once
more with the other reading of the hash. eMule's Kad flood protection takes one
KADEMLIA_CALLBACK_REQ per minute from an IP, so no IP gets a second request sooner; a switch to
another buddy IP does not wait. The callback socket still times out after 40 s: the source waits
to ask, not in a socket. Once every buddy has had both layouts, the route has failed and no further
request follows.

With a second copy of the source naming another buddy, that buddy is tried first, in both layouts,
and then the first one in the layout it was not asked in.

One fake Kad node is the daemon's only contact and also the source's buddy. Neither buddy relays.
The argument after the daemon picks the case, so each fits the CI time limit: 'one' or 'two'.
"""
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

from AllSearchIntegrationTest import C, integer, retry_port_collisions, string, wait_for
from KadLanModeIntegrationTest import write_private_contacts
from KadSourceBuddyIdIntegrationTest import (FILE_HASH, FILE_SIZE, KADEMLIA_CALLBACK_REQ, NODE_ID,
                                             NODE_VERSION, SOURCE_ID, SOURCE_PORT, FakeNode,
                                             kad_ip, private_address, source_tags)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_RES, KADEMLIA2_SEARCH_SOURCE_REQ,
                                 TAG_BUDDYHASH, TAG_SERVERIP, TAG_SERVERPORT, running_kad_daemon,
                                 tag as kad_tag, unpack_kad_packet)

# No 4-byte group is a palindrome, so the two layouts differ.
BUDDY_HASH = b'00112233445566778899aabbccddeeff'
# eMule drops a second request from an IP within a minute of the last one.
FLOOD_INTERVAL = 60
# A wrong request to the same buddy would follow the last one by its 65 s wait. Only the one-buddy
# case waits for it: the two-buddy case needs three such waits already, and its last request ends
# the route the same way.
SETTLE_SECONDS = 75
# The next request to the same IP goes out this long after the last one; the socket gives up at 40 s.
SAME_IP_WAIT = 65
SOCKET_TIMEOUT = 40
DS_NONE = 13
SHORT_SETTLE_SECONDS = 20


def other_layout(buddy_id):
    return b''.join(buddy_id[i:i + 4][::-1] for i in range(0, 16, 4))


def record(address, port):
    """A firewalled source reached through the buddy at address:port."""
    return source_tags(False) + [kad_tag(TAG_BUDDYHASH, 0x02, BUDDY_HASH),
                                 kad_tag(TAG_SERVERIP, 0x03, kad_ip(address)),
                                 kad_tag(TAG_SERVERPORT, 0x08, port)]


class SecondBuddy(threading.Thread):
    """A buddy that is no Kad contact: it only records the callback requests it gets."""

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
                self.requests.append((time.monotonic(), 'second', packet[2:18]))

    def stop(self):
        self.running = False
        self.join(timeout=5)
        self.sock.close()


class SilentBuddy(FakeNode):
    """Serves a source whose buddy is this node, and maybe a copy naming the second buddy."""

    def __init__(self, address, second=None):
        super().__init__(address, with_buddy_hash=False)
        self.second = second
        self.requests = second.requests if second else []

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            self.source_searches += 1
            address = self.sock.getsockname()[0]
            records = [record(address, self.port)]
            if self.second:
                records.append(record(address, self.second.port))
            entries = b''.join(SOURCE_ID + bytes([len(tags)]) + b''.join(tags) for tags in records)
            self.send(peer, KADEMLIA2_SEARCH_RES, NODE_ID + body[:16]
                      + struct.pack('<H', len(records)) + entries)
        elif opcode == KADEMLIA_CALLBACK_REQ:
            self.requests.append((time.monotonic(), 'first', body[:16]))
        else:
            super().handle(opcode, body, peer)


def poll_sources(ec, sources):
    """Merge the clients of one incremental EC update into sources: id -> {tag: value}."""
    opcode, tags = ec.call_list(C['EC_OP_GET_UPDATE'], [
        integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_INC_UPDATE'])])
    assert opcode == C['EC_OP_SHARED_FILES'], opcode
    for name, _, children in tags:
        if name == C['EC_TAG_CLIENT']:
            for _, client_id, fields in children:
                sources.setdefault(int.from_bytes(client_id, 'big'), {}).update(
                    {key: int.from_bytes(data, 'big') for key, data, _ in fields
                     if key in (C['EC_TAG_CLIENT_DOWNLOAD_STATE'], C['EC_TAG_CLIENT_NEXT_ASK_AT'])})


def check_waits_to_ask(proc, ec, first_request):
    """Between the socket timeout and the next request, the source waits to ask, until then."""
    sent_at = time.time() - (time.monotonic() - first_request)
    time.sleep(max(0.0, first_request + SOCKET_TIMEOUT + 6 - time.monotonic()))
    sources = {}
    poll_sources(ec, sources)
    waiting = [fields for fields in sources.values()
               if fields.get(C['EC_TAG_CLIENT_DOWNLOAD_STATE']) == DS_NONE]
    assert waiting, ('the source does not wait to ask after its callback socket timed out', sources)
    ask_at = waiting[0].get(C['EC_TAG_CLIENT_NEXT_ASK_AT'], 0)
    assert abs(ask_at - (sent_at + SAME_IP_WAIT)) < 3, (
        'the next ask is not the retry time', ask_at - sent_at)
    print(f'waiting to ask {ask_at - sent_at:.1f} s after the first request')


def drive(binary, address, expected, with_second, settle):
    """Feed one daemon the source and return the callback requests as (time, buddy, ID)."""
    second = SecondBuddy(address, []) if with_second else None
    node = SilentBuddy(address, second)
    threads = [thread for thread in (node, second) if thread]
    for thread in threads:
        thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='kad-callback-retry-') as directory:
            root = Path(directory)
            write_private_contacts(root, 1, address=address, udp_port=node.port,
                                   tcp_port=SOURCE_PORT, version=NODE_VERSION)
            with running_kad_daemon(binary, root, FilterLanIPs=0) as (proc, ec, _udp, _peer):
                wait_for(proc, 'the fake node to be asked for contacts',
                         lambda: len(node.lookups) > 0, 60)
                link = f'ed2k://|file|callback-retry.bin|{FILE_SIZE}|{FILE_HASH}|/'
                assert ec.call(C['EC_OP_ADD_LINK'],
                               [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                added = time.monotonic()
                wait_for(proc, 'a callback request through the published buddy',
                         lambda: bool(node.requests), 180)
                print(f'first callback request {node.requests[0][0] - added:.1f} s after the link')
                check_waits_to_ask(proc, ec, node.requests[0][0])
                for count in range(2, expected + 1):
                    wait_for(proc, f'callback request {count} of {expected}',
                             lambda: len(node.requests) >= count, FLOOD_INTERVAL + 30)
                try:
                    wait_for(proc, 'a callback request after every layout was tried',
                             lambda: len(node.requests) > expected, settle)
                except AssertionError:
                    pass
    finally:
        for thread in threads:
            thread.stop()
    return node.requests


def check(requests, expected):
    """Every buddy is asked in both layouts, never twice within a minute, and then no more."""
    print('requests:', ', '.join(f'{t - requests[0][0]:.1f} s {buddy} {ident[:4].hex()}'
                                 for t, buddy, ident in requests))
    assert len(requests) == expected, ('not every buddy and layout tried exactly once',
                                       [(buddy, ident.hex()) for _, buddy, ident in requests])
    first_id = requests[0][2]
    buddies = {buddy for _, buddy, _ in requests}
    pairs = {(buddy, ident) for _, buddy, ident in requests}
    assert pairs == {(b, i) for b in buddies for i in (first_id, other_layout(first_id))}, pairs
    # Both buddies share one IP, and eMule's limit is per IP.
    times = [t for t, _, _ in requests]
    for previous, current in zip(times, times[1:]):
        assert current - previous > FLOOD_INTERVAL, (
            f'the buddy IP was asked again {current - previous:.1f} s after its last request')


def run(binary, case):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    if case == 'one':
        check(drive(binary, address, 2, with_second=False, settle=SETTLE_SECONDS), 2)
        print('PASS: one buddy is asked in both layouts, a minute apart, and then no more')
        return

    # The buddy asked first is the stored one; the copy naming the other is the candidate.
    # Both buddies share our address, so even a switch waits out the flood interval.
    two_buddies = drive(binary, address, 4, with_second=True, settle=SHORT_SETTLE_SECONDS)
    check(two_buddies, 4)
    order = [buddy for _, buddy, _ in two_buddies]
    assert order[0] == order[3] and order[1] == order[2] != order[0], order
    assert two_buddies[1][2] == two_buddies[0][2], 'the candidate was not asked in the same layout'
    print('PASS: with a candidate, the candidate in both layouts and then the first buddy in the '
          'other one; no IP twice within a minute')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[2] not in ('one', 'two'):
        sys.exit(f'usage: {sys.argv[0]} AMULED one|two')
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), sys.argv[2])
