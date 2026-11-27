#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A firewalled node asks a LowID Kad source's buddy for a rendezvous, since nothing else reaches it.

The daemon's only live Kad contact is a fake node on a private address. A second contact, public and
unreachable, keeps Kad out of LAN mode, so the daemon stays firewalled: nobody answers its firewall
check. The fake node hands out one type 3 source whose buddy is a UDP socket of this test. With no
ed2k server, the three server paths have nothing to offer, so the daemon sends the buddy an
OP_REASKCALLBACKUDP (0x94) carrying an OP_RENDEZVOUS (0xA0), three times 5 s apart, and dials the
source's Kad endpoint once when no punch comes. The source's address is documentation space, so no
punch ever comes back and no dial connects.
"""
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadBuddyLifecycleIntegrationTest import buddy_counters
from KadSourceBuddyIdIntegrationTest import (BUDDY_HASH, FILE_HASH, FILE_SIZE, NODE_VERSION, SOURCE_ADDRESS,
                                             SOURCE_PORT, FakeNode, kad_ip, private_address)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_SOURCE_REQ, TAG_BUDDYHASH, TAG_FILESIZE, TAG_SERVERIP,
                                 TAG_SERVERPORT, TAG_SOURCEIP, TAG_SOURCEPORT, TAG_SOURCETYPE,
                                 running_kad_daemon)
from KadSourceTagsECTest import tag as kad_tag

TAG_SOURCEUPORT = b'\xfc'
SOURCE_UDP_PORT = 4672
# TEST-NET-2 again: public to IsGoodIPPort(), so Kad does not run in LAN mode, and never answers.
UNREACHABLE_CONTACT = '198.51.100.9'
OP_EMULEPROT = 0xC5
OP_REASKCALLBACKUDP = 0x94
OP_RENDEZVOUS = 0xA0
NATT_UTP, NATT_QUIC, ENDPOINT_HINT_BIT = 0x80, 0x40, 0x20
HINT_UTP, HINT_QUIC = 1, 2
# <buddy id 16><zero 16><0xA0 1><our hash 16><opts 1><file hash 16><our ip 4><udp port 2><hint 1>
ENVELOPE_SIZE = 16 + 16 + 1 + 16 + 1 + 16 + 4 + 2 + 1
SENDS = 3
SEND_INTERVAL = 5


def write_contacts(root, fake_address, fake_port):
    """nodes.dat with the fake node and one public contact that never answers."""
    contacts = b''
    for index, (address, udp_port) in enumerate(((fake_address, fake_port), (UNREACHABLE_CONTACT, 4672))):
        ip = int.from_bytes(socket.inet_aton(address), 'big')
        contacts += struct.pack('<4IIHHB2IB', 0xA0000000 + index, 0, 0, 1, ip, udp_port, SOURCE_PORT,
                                NODE_VERSION, 0, 0, 1)
    (Path(root) / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 2) + contacts)


class Buddy:
    """The source's buddy: records each 0x94 the daemon sends it, with its arrival time."""

    def __init__(self, address):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((address, 0))
        self.sock.settimeout(0.2)
        self.address, self.port = self.sock.getsockname()
        self.reasks = []

    def drain(self):
        while True:
            try:
                packet = self.sock.recv(65535)
            except TimeoutError:
                return
            if packet[:2] == bytes([OP_EMULEPROT, OP_REASKCALLBACKUDP]):
                self.reasks.append((time.monotonic(), packet[2:]))

    def close(self):
        self.sock.close()


class SourceNode(FakeNode):
    """The fake node, handing out a type 3 source that names our buddy socket."""

    def __init__(self, address, buddy):
        super().__init__(address, with_buddy_hash=True)
        self.buddy = buddy

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            self.source_searches += 1
            tags = [kad_tag(TAG_SOURCETYPE, 0x09, 3),
                    kad_tag(TAG_SOURCEIP, 0x03, kad_ip(SOURCE_ADDRESS)),
                    kad_tag(TAG_SOURCEPORT, 0x08, SOURCE_PORT),
                    kad_tag(TAG_SOURCEUPORT, 0x08, SOURCE_UDP_PORT),
                    kad_tag(TAG_FILESIZE, 0x03, FILE_SIZE),
                    kad_tag(TAG_SERVERIP, 0x03, kad_ip(self.buddy.address)),
                    kad_tag(TAG_SERVERPORT, 0x08, self.buddy.port),
                    kad_tag(TAG_BUDDYHASH, 0x02, BUDDY_HASH)]
            self.send_records(peer, body[:16], [tags])
        else:
            super().handle(opcode, body, peer)


def check_envelope(body, udp_port):
    assert len(body) == ENVELOPE_SIZE, ('envelope size', len(body), body.hex())
    # The buddy ID the source published, in either layout: 16 equal bytes read the same both ways.
    assert body[:16] == b'\x5b' * 16, ('buddy id', body[:16].hex())
    assert body[16:32] == bytes(16), ('zero hash marker', body[16:32].hex())
    assert body[32] == OP_RENDEZVOUS, ('opcode', body[32])
    assert body[33:49] != bytes(16), 'no requester hash'
    options = body[49]
    assert options & NATT_UTP, ('uTP not declared', hex(options))
    assert not options & ENDPOINT_HINT_BIT, ('endpoint hint bit set', hex(options))
    assert body[50:66] == bytes.fromhex(FILE_HASH), ('file hash', body[50:66].hex())
    port, hint = struct.unpack('<HB', body[70:73])
    assert port == udp_port, ('tail port', port, udp_port)
    expected_hint = HINT_QUIC if options & NATT_QUIC else HINT_UTP
    assert hint == expected_hint, ('tail hint', hint, hex(options))


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    buddy = Buddy(address)
    node = SourceNode(address, buddy)
    node.start()
    try:
        with tempfile.TemporaryDirectory(prefix='kad-rendezvous-requester-') as directory:
            root = Path(directory)
            write_contacts(root, address, node.port)
            with running_kad_daemon(binary, root, FilterLanIPs=0) as (proc, ec, udp_port, _peer):
                wait_for(proc, 'the fake node to be asked for contacts', lambda: len(node.lookups) > 0, 60)
                link = f'ed2k://|file|rendezvous.bin|{FILE_SIZE}|{FILE_HASH}|/'
                assert ec.call(C['EC_OP_ADD_LINK'],
                               [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                wait_for(proc, 'the daemon to search Kad for sources of the file',
                         lambda: node.source_searches > 0, 180)

                def sent(count):
                    buddy.drain()
                    return len(buddy.reasks) >= count

                wait_for(proc, 'a rendezvous request to the source\'s buddy', lambda: sent(1), 120)
                wait_for(proc, f'{SENDS} rendezvous requests', lambda: sent(SENDS),
                         SENDS * SEND_INTERVAL + 10)
                # Nothing more is due within the attempt: no fourth send.
                time.sleep(SEND_INTERVAL + 2)
                buddy.drain()
                counters = buddy_counters(ec)
    finally:
        node.stop()
        buddy.close()

    assert len(buddy.reasks) == SENDS, ('sends', len(buddy.reasks))
    for _, body in buddy.reasks:
        check_envelope(body, udp_port)
    gaps = [later - earlier for (earlier, _), (later, _) in zip(buddy.reasks, buddy.reasks[1:], strict=False)]
    assert all(SEND_INTERVAL - 1.5 <= gap <= SEND_INTERVAL + 2.5 for gap in gaps), ('send gaps', gaps)

    assert counters['OWN_REQUESTED_RENDEZVOUS_STARTED'] == 1, counters
    assert counters['OWN_REQUESTED_RENDEZVOUS_SENT'] == SENDS, counters
    assert counters['OWN_REQUESTED_RENDEZVOUS_PUNCHED'] == 0, counters
    assert counters['OWN_REQUESTED_RENDEZVOUS_FALLBACK_DIALS'] == 1, counters
    assert counters['OWN_REQUESTED_RENDEZVOUS_CONNECTED'] == 0, counters
    assert counters['OWN_REQUESTED_RENDEZVOUS_NO_RENDEZVOUS'] == 0, counters
    # The served side counts nothing: this node asked, it was not asked.
    assert counters['OWN_RENDEZVOUS_RECEIVED'] == 0, counters
    print('PASS: a firewalled node asks a Kad source\'s buddy for a rendezvous three times, 5 s '
          'apart, in the sketched layout, and dials the source once when no punch comes')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
