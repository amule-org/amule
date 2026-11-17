"""An unanswered callback through a source's buddy is retried in the other buddy ID layout.

A source published before #1993 by aMule wrote TAG_BUDDYHASH in another byte order, and an eMule
family buddy only relays the ID it registered. So when a KADEMLIA_CALLBACK_REQ goes unanswered,
the downloader asks the same buddy once more with the other reading of the hash. eMule's Kad flood
protection takes one KADEMLIA_CALLBACK_REQ per minute from an IP, so no request follows the
previous one sooner. Once every buddy has had both layouts, the route has failed and no further
request follows.

With a second copy of the source naming another buddy, that buddy is tried first, in both layouts,
and then the first one in the layout it was not asked in.

One fake Kad node is the daemon's only contact and also the source's buddy. Neither buddy relays.
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
                                             NODE_VERSION, SOURCE_ID, SOURCE_PORT, FakeNode,
                                             kad_ip, private_address, source_tags)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_RES, KADEMLIA2_SEARCH_SOURCE_REQ,
                                 TAG_BUDDYHASH, TAG_SERVERIP, TAG_SERVERPORT, running_kad_daemon,
                                 tag as kad_tag, unpack_kad_packet)

# No 4-byte group is a palindrome, so the two layouts differ.
BUDDY_HASH = b'00112233445566778899aabbccddeeff'
FLOOD_INTERVAL = 60
# A further request would follow the last one's timeout and the next process tick.
SETTLE_SECONDS = 80


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


def drive(binary, address, expected, with_second):
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
                wait_for(proc, 'a callback request through the published buddy',
                         lambda: bool(node.requests), 180)
                for count in range(2, expected + 1):
                    wait_for(proc, f'callback request {count} of {expected}',
                             lambda: len(node.requests) >= count, FLOOD_INTERVAL + 60)
                try:
                    wait_for(proc, 'a callback request after every layout was tried',
                             lambda: len(node.requests) > expected, SETTLE_SECONDS)
                except AssertionError:
                    pass
    finally:
        for thread in threads:
            thread.stop()
    return node.requests


def check(requests, expected):
    """Every buddy is asked in both layouts, never twice within a minute, and then no more."""
    assert len(requests) == expected, ('not every buddy and layout tried exactly once',
                                       [(buddy, ident.hex()) for _, buddy, ident in requests])
    first_id = requests[0][2]
    buddies = {buddy for _, buddy, _ in requests}
    pairs = {(buddy, ident) for _, buddy, ident in requests}
    assert pairs == {(b, i) for b in buddies for i in (first_id, other_layout(first_id))}, pairs
    for previous, current in zip(requests, requests[1:]):
        gap = current[0] - previous[0]
        assert gap >= FLOOD_INTERVAL - 1, f'a request {gap:.1f} s after the previous one'


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    one_buddy = drive(binary, address, 2, with_second=False)
    check(one_buddy, 2)

    # The buddy asked first is the stored one; the copy naming the other is the candidate.
    two_buddies = drive(binary, address, 4, with_second=True)
    check(two_buddies, 4)
    order = [buddy for _, buddy, _ in two_buddies]
    assert order[0] == order[3] and order[1] == order[2] != order[0], order
    assert two_buddies[1][2] == two_buddies[0][2], 'the candidate was not asked in the same layout'
    print('PASS: one buddy is asked in both layouts; with a candidate, the candidate in both and '
          'then the first buddy in the other layout; never twice within a minute')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
