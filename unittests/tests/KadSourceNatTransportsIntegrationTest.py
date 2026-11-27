#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Kad source record's NAT-T transports are kept for the source.

eMuleAI declares them in TAG_ENCRYPTION (0x80 uTP, 0x40 QUIC), eMuleQt in its "nat" tag (bit 0,
QUIC). One fake Kad node, the daemon's only contact, answers the source search with a record that
declares nothing, then one of each kind. The daemon logs what each source declares.
"""
import struct
import sys
import tempfile
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadLanModeIntegrationTest import write_private_contacts
from KadSourceBuddyIdIntegrationTest import (FILE_HASH, FILE_SIZE, NODE_ID, NODE_VERSION, SOURCE_ADDRESS,
                                             SOURCE_PORT, FakeNode, kad_ip, private_address)
from KadSourceTagsECTest import (KADEMLIA2_SEARCH_RES, KADEMLIA2_SEARCH_SOURCE_REQ, TAG_ENCRYPTION, TAG_FILESIZE,
                                 TAG_SOURCEIP, TAG_SOURCEPORT, TAG_SOURCETYPE, running_kad_daemon)
from KadSourceTagsECTest import tag as kad_tag

TAG_NATT = b'nat'
# (source ID, TAG_ENCRYPTION, nat tag or None, what the log names). Each ID reads the same as a Kad
# ID and as a user hash. The record that declares nothing comes first, so once the others are
# logged it has been handled too.
SOURCES = [
    (b'\x44' * 16, 0x01, None, None),
    (b'\x11' * 16, 0x80 | 0x40 | 0x01, None, 'uTP and QUIC'),
    (b'\x22' * 16, 0x01, 0x01, 'QUIC'),
    (b'\x33' * 16, 0x80, None, 'uTP'),
]


def record_tags(crypt, nat):
    """A firewalled source (type 3) with no buddy route, so the daemon contacts nobody."""
    tags = [kad_tag(TAG_SOURCETYPE, 0x09, 3),
            kad_tag(TAG_SOURCEIP, 0x03, kad_ip(SOURCE_ADDRESS)),
            kad_tag(TAG_SOURCEPORT, 0x08, SOURCE_PORT),
            kad_tag(TAG_FILESIZE, 0x03, FILE_SIZE),
            kad_tag(TAG_ENCRYPTION, 0x09, crypt)]
    if nat is not None:
        tags.append(kad_tag(TAG_NATT, 0x09, nat))
    return tags


class TransportsNode(FakeNode):
    """The daemon's only Kad contact, answering a source search with SOURCES."""

    def __init__(self, address):
        super().__init__(address, with_buddy_hash=False)

    def handle(self, opcode, body, peer):
        if opcode != KADEMLIA2_SEARCH_SOURCE_REQ:
            super().handle(opcode, body, peer)
            return
        self.source_searches += 1
        entries = b''
        for source_id, crypt, nat, _ in SOURCES:
            tags = record_tags(crypt, nat)
            entries += source_id + bytes([len(tags)]) + b''.join(tags)
        self.send(peer, KADEMLIA2_SEARCH_RES, NODE_ID + body[:16] + struct.pack('<H', len(SOURCES)) + entries)


def declared(source_id, names):
    return f'Kad source {source_id.hex().upper()} declares NAT-T over {names}'


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    node = TransportsNode(address)
    node.start()
    try:
        with tempfile.TemporaryDirectory(prefix='kad-source-natt-') as directory:
            root = Path(directory)
            write_private_contacts(root, 1, address=address, udp_port=node.port,
                                   tcp_port=SOURCE_PORT, version=NODE_VERSION)
            with running_kad_daemon(binary, root, console='stdout.log', FilterLanIPs=0, VerboseDebug=1,
                                    VerboseDebugLogfile=1,
                                    sections={'Debug': {'Cat_Kademlia Search': 1}}) as (proc, ec, _udp, _peer):
                wait_for(proc, 'the fake node to be asked for contacts', lambda: len(node.lookups) > 0, 60)
                link = f'ed2k://|file|natt.bin|{FILE_SIZE}|{FILE_HASH}|/'
                assert ec.call(C['EC_OP_ADD_LINK'],
                               [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                wait_for(proc, 'the daemon to search Kad for sources of the file',
                         lambda: node.source_searches > 0, 180)

                def log():
                    return (root / 'logfile').read_text(errors='replace')

                for source_id, _, _, names in SOURCES:
                    if names is not None:
                        wait_for(proc, f'the log line {declared(source_id, names)!r}',
                                 lambda line=declared(source_id, names): line in log(), 30)
                silent = f'Kad source {SOURCES[0][0].hex().upper()} declares'
                assert silent not in log(), 'a record with no NAT-T bit was logged as declaring one'
    finally:
        node.stop()
    print('PASS: a Kad source keeps the NAT-T transports its record declares: eMuleAI\'s bits in '
          'TAG_ENCRYPTION and eMuleQt\'s nat tag; a record with neither declares none')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
