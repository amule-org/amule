"""An unanswered callback through a source's buddy is retried once in the other buddy ID layout.

A source published before #1993 by aMule wrote TAG_BUDDYHASH in another byte order, and an eMule
family buddy only relays the ID it registered. So when a KADEMLIA_CALLBACK_REQ goes unanswered,
the downloader asks the same buddy once more with the other reading of the hash. eMule's Kad flood
protection takes one KADEMLIA_CALLBACK_REQ per minute from an IP, so the second request must not
go out sooner. After it, the route has failed and no third request follows.

One fake Kad node is the daemon's only contact and also the source's buddy. It never relays.
"""
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadLanModeIntegrationTest import write_private_contacts
from KadSourceBuddyIdIntegrationTest import (FILE_HASH, FILE_SIZE, KADEMLIA_CALLBACK_REQ, NODE_ID,
                                             NODE_VERSION, SOURCE_ID, SOURCE_PORT, FakeNode,
                                             kad_ip, private_address, source_tags)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_RES, KADEMLIA2_SEARCH_SOURCE_REQ,
                                 TAG_BUDDYHASH, TAG_SERVERIP, TAG_SERVERPORT, running_kad_daemon,
                                 tag as kad_tag)

# No 4-byte group is a palindrome, so the two layouts differ.
BUDDY_HASH = b'00112233445566778899aabbccddeeff'
FLOOD_INTERVAL = 60
# A third request would follow the second's timeout and the next process tick.
SETTLE_SECONDS = 90


def other_layout(buddy_id):
    return b''.join(buddy_id[i:i + 4][::-1] for i in range(0, 16, 4))


class SilentBuddy(FakeNode):
    """Serves a source whose buddy is this node, and records the callback requests it gets."""

    def __init__(self, address):
        super().__init__(address, with_buddy_hash=False)
        self.callback_times = []

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            self.source_searches += 1
            tags = source_tags(False) + [
                kad_tag(TAG_BUDDYHASH, 0x02, BUDDY_HASH),
                kad_tag(TAG_SERVERIP, 0x03, kad_ip(self.sock.getsockname()[0])),
                kad_tag(TAG_SERVERPORT, 0x08, self.port)]
            self.send(peer, KADEMLIA2_SEARCH_RES,
                      NODE_ID + body[:16] + struct.pack('<H', 1) + SOURCE_ID
                      + bytes([len(tags)]) + b''.join(tags))
        elif opcode == KADEMLIA_CALLBACK_REQ:
            self.callback_times.append(time.monotonic())
            super().handle(opcode, body, peer)
        else:
            super().handle(opcode, body, peer)


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    node = SilentBuddy(address)
    node.start()
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
                         lambda: bool(node.callbacks), 180)
                wait_for(proc, 'a second callback request in the other layout',
                         lambda: len(node.callbacks) > 1, FLOOD_INTERVAL + 60)
                try:
                    wait_for(proc, 'a third callback request', lambda: len(node.callbacks) > 2,
                             SETTLE_SECONDS)
                except AssertionError:
                    pass
    finally:
        node.stop()

    first, second = node.callbacks[0][:16], node.callbacks[1][:16]
    assert second == other_layout(first), ('second request not in the other layout',
                                           first.hex(), second.hex())
    gap = node.callback_times[1] - node.callback_times[0]
    assert gap >= FLOOD_INTERVAL - 1, f'second request after {gap:.1f} s, inside the flood limit'
    assert len(node.callbacks) == 2, ('the route did not end after both layouts',
                                      [body[:16].hex() for body in node.callbacks])
    print(f'PASS: the unanswered callback was retried once in the other layout after {gap:.1f} s')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
