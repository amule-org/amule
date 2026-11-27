#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A firewalled node asks a LowID Kad source's buddy for a rendezvous, since nothing else reaches it.

The daemon's only live Kad contact is a fake node on a private address. A second contact, public and
unreachable, keeps Kad out of LAN mode, so the daemon stays firewalled: nobody answers its firewall
check. The fake node hands out one type 3 source whose buddy and whose Kad endpoint are UDP sockets
of this test. With no ed2k server, the three server paths have nothing to offer, so the daemon sends
the buddy an OP_REASKCALLBACKUDP (0x94) carrying an OP_RENDEZVOUS (0xA0) and punches the source.

Four cases: a silent source gets three requests 5 s apart and one fallback dial; a source that
punches back ends the requests and gets no fallback dial; a stale record of the source that comes
last does not stop the punches to its current endpoint; a source that answers OP_FILENOTFOUND does
not speak the rendezvous, and stays a source.
"""
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadBuddyLifecycleIntegrationTest import buddy_counters
from KadSourceBuddyIdIntegrationTest import (
    FILE_HASH,
    FILE_SIZE,
    NODE_VERSION,
    SOURCE_PORT,
    FakeNode,
    kad_ip,
    private_address,
)
from KadSourceTagsECTest import (
    KADEMLIA2_SEARCH_SOURCE_REQ,
    TAG_BUDDYHASH,
    TAG_FILESIZE,
    TAG_SERVERIP,
    TAG_SERVERPORT,
    TAG_SOURCEIP,
    TAG_SOURCEPORT,
    TAG_SOURCETYPE,
    running_kad_daemon,
)
from KadSourceTagsECTest import tag as kad_tag

TAG_SOURCEUPORT = b'\xfc'
# TEST-NET-2: public to IsGoodIPPort(), so Kad does not run in LAN mode, and never answers.
UNREACHABLE_CONTACT = '198.51.100.9'
# Distinct bytes, so the layout the buddy ID goes back in shows on the wire.
BUDDY_ID = bytes(range(0x10, 0x20))
# What a stock buddy compares: the ID as eMule's requester reads the tag into a CUInt128 and sends its
# ToByteArray(), which reverses each 4-byte group of the tag text's bytes.
BUDDY_ID_ON_WIRE = b''.join(BUDDY_ID[i:i + 4][::-1] for i in range(0, 16, 4))
OP_EMULEPROT = 0xC5
OP_REASKCALLBACKUDP = 0x94
OP_FILENOTFOUND = 0x92
OP_HOLEPUNCH = 0xA1
OP_RENDEZVOUS = 0xA0
NATT_UTP, NATT_QUIC, ENDPOINT_HINT_BIT = 0x80, 0x40, 0x20
HINT_UTP, HINT_QUIC = 1, 2
# <buddy id 16><zero 16><0xA0 1><our hash 16><opts 1><file hash 16><our ip 4><udp port 2><hint 1>
ENVELOPE_SIZE = 16 + 16 + 1 + 16 + 1 + 16 + 4 + 2 + 1
SENDS = 3
SEND_INTERVAL = 5
# The daemon listens on loopback. A socket bound to the private address still reaches it from that
# address, as the fake node's answers do, so the daemon sees the source's own endpoint.
DAEMON_ADDRESS = '127.0.0.1'


def write_contacts(root, fake_address, fake_port):
    """nodes.dat with the fake node and one public contact that never answers."""
    contacts = b''
    for index, (address, udp_port) in enumerate(((fake_address, fake_port), (UNREACHABLE_CONTACT, 4672))):
        ip = int.from_bytes(socket.inet_aton(address), 'big')
        contacts += struct.pack('<4IIHHB2IB', 0xA0000000 + index, 0, 0, 1, ip, udp_port, SOURCE_PORT,
                                NODE_VERSION, 0, 0, 1)
    (Path(root) / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 2) + contacts)


class Endpoint:
    """A UDP socket of the test that records the eMule-protocol packets the daemon sends it."""

    def __init__(self, address):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((address, 0))
        self.sock.settimeout(0.2)
        self.address, self.port = self.sock.getsockname()
        self.packets = []

    def drain(self):
        while True:
            try:
                packet = self.sock.recv(65535)
            except TimeoutError:
                return
            if packet[:1] == bytes([OP_EMULEPROT]) and len(packet) >= 2:
                self.packets.append((time.monotonic(), packet[1], packet[2:]))

    def received(self, opcode):
        self.drain()
        return [(at, body) for at, op, body in self.packets if op == opcode]

    def close(self):
        self.sock.close()


class SourceNode(FakeNode):
    """The fake node, handing out a type 3 source at the source socket, behind the buddy socket."""

    def __init__(self, address, buddy, source, stale_port=None):
        super().__init__(address, with_buddy_hash=True)
        self.buddy = buddy
        self.source = source
        self.stale_port = stale_port

    def record(self, udp_port):
        # TAG_SOURCEIP in host order, as Kad carries it; TAG_SERVERIP as aMule stores it.
        source_ip = int.from_bytes(socket.inet_aton(self.source.address), 'big')
        return [kad_tag(TAG_SOURCETYPE, 0x09, 3),
                kad_tag(TAG_SOURCEIP, 0x03, source_ip),
                kad_tag(TAG_SOURCEPORT, 0x08, SOURCE_PORT),
                kad_tag(TAG_SOURCEUPORT, 0x08, udp_port),
                kad_tag(TAG_FILESIZE, 0x03, FILE_SIZE),
                kad_tag(TAG_SERVERIP, 0x03, kad_ip(self.buddy.address)),
                kad_tag(TAG_SERVERPORT, 0x08, self.buddy.port),
                kad_tag(TAG_BUDDYHASH, 0x02, BUDDY_ID.hex().encode())]

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            self.source_searches += 1
            records = [self.record(self.source.port)]
            if self.stale_port is not None:
                # As after the source restarted: a record from before still comes back, last.
                records.append(self.record(self.stale_port))
            self.send_records(peer, body[:16], records)
        else:
            super().handle(opcode, body, peer)


def check_envelope(body, udp_port):
    assert len(body) == ENVELOPE_SIZE, ('envelope size', len(body), body.hex())
    assert body[:16] == BUDDY_ID_ON_WIRE, ('buddy id not as a stock buddy compares it', body[:16].hex())
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


def source_count(ec):
    opcode, tags = ec.call_list(C['EC_OP_GET_DLOAD_QUEUE'])
    assert opcode == C['EC_OP_DLOAD_QUEUE'], opcode
    for name, _, children in tags:
        if name == C['EC_TAG_PARTFILE']:
            fields = {key: value for key, value, _ in children}
            return int.from_bytes(fields[C['EC_TAG_PARTFILE_SOURCE_COUNT']], 'big')
    raise AssertionError(('no part file', tags))


def drive(binary, address, answer, stale_record=False):
    """One daemon and one source. answer is None, OP_HOLEPUNCH or OP_FILENOTFOUND: what the source
    sends the daemon after the first request reaches its buddy. stale_record adds an older record of
    the source last, at a port well outside the sweep around the current one."""
    buddy = Endpoint(address)
    source = Endpoint(address)
    stale_port = None
    if stale_record:
        stale_port = source.port + 4000 if source.port < 60000 else source.port - 4000
    node = SourceNode(address, buddy, source, stale_port)
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
                wait_for(proc, 'a rendezvous request to the source\'s buddy',
                         lambda: buddy.received(OP_REASKCALLBACKUDP), 120)
                if answer == OP_HOLEPUNCH:
                    source.sock.sendto(bytes([OP_EMULEPROT, OP_HOLEPUNCH]), (DAEMON_ADDRESS, udp_port))
                elif answer == OP_FILENOTFOUND:
                    before = source_count(ec)
                    assert before == 1, ('sources before the answer', before)
                    source.sock.sendto(bytes([OP_EMULEPROT, OP_FILENOTFOUND]) + bytes.fromhex(FILE_HASH),
                                       (DAEMON_ADDRESS, udp_port))
                # Past the last send and the fallback dial of the attempt. Read as it goes, since a
                # packet's time is taken when it is read.
                end = time.monotonic() + SENDS * SEND_INTERVAL + 2
                while time.monotonic() < end:
                    buddy.drain()
                    source.drain()
                reasks = buddy.received(OP_REASKCALLBACKUDP)
                punches = source.received(OP_HOLEPUNCH)
                counters = buddy_counters(ec)
                sources = source_count(ec)
    finally:
        node.stop()
        buddy.close()
        source.close()
    return reasks, punches, counters, sources, udp_port


def requested(counters):
    prefix = 'OWN_REQUESTED_RENDEZVOUS_'
    return {name[len(prefix):].lower(): value for name, value in counters.items() if name.startswith(prefix)}


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    reasks, punches, counters, _sources, udp_port = drive(binary, address, None)
    assert len(reasks) == SENDS, ('sends to a silent source', len(reasks))
    for _, body in reasks:
        check_envelope(body, udp_port)
    gaps = [later - earlier for (earlier, _), (later, _) in zip(reasks, reasks[1:], strict=False)]
    assert all(SEND_INTERVAL - 1.5 <= gap <= SEND_INTERVAL + 2.5 for gap in gaps), ('send gaps', gaps)
    assert punches, 'the daemon never punched the source'
    assert requested(counters) == {'started': 1, 'sent': SENDS, 'punched': 0, 'fallback_dials': 1,
                                   'connected': 0, 'timed_out': 0, 'no_rendezvous': 0}, counters
    # The served side counts nothing: this node asked, it was not asked.
    assert counters['OWN_RENDEZVOUS_RECEIVED'] == 0, counters

    reasks, punches, counters, _sources, _ = drive(binary, address, OP_HOLEPUNCH)
    assert len(reasks) == 1, ('requests after the source punched back', len(reasks), requested(counters),
                              len(punches))
    assert requested(counters) == {'started': 1, 'sent': 1, 'punched': 1, 'fallback_dials': 0,
                                   'connected': 0, 'timed_out': 0, 'no_rendezvous': 0}, counters

    # The stale record arrives last, so its endpoint is the one kept; the current one is punched too.
    reasks, punches, counters, _sources, _ = drive(binary, address, None, stale_record=True)
    assert len(reasks) == SENDS, ('sends with a stale record last', len(reasks))
    assert punches, 'the endpoint of the current record was not punched'
    assert requested(counters)['started'] == 1, counters

    reasks, _punches, counters, sources, _ = drive(binary, address, OP_FILENOTFOUND)
    assert len(reasks) == 1, ('requests after OP_FILENOTFOUND', len(reasks))
    assert requested(counters) == {'started': 1, 'sent': 1, 'punched': 0, 'fallback_dials': 0,
                                   'connected': 0, 'timed_out': 0, 'no_rendezvous': 1}, counters
    assert sources == 1, ('a source that does not speak the rendezvous was dropped', sources)
    print('PASS: a firewalled node asks a Kad source\'s buddy for a rendezvous in the sketched layout, '
          'with the buddy ID as a stock buddy compares it; a silent source gets three requests and one fallback dial, '
          'a punch ends the requests, a stale record that comes last does not stop the punches to the current endpoint, '
          'and OP_FILENOTFOUND ends the attempt and keeps the source')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
