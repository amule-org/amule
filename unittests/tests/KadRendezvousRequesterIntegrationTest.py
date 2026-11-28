#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A firewalled node asks a LowID Kad source's buddy for a rendezvous, since nothing else reaches it.

The daemon's only live Kad contact is a fake node on a private address. A second contact, public and
unreachable, keeps Kad out of LAN mode, so the daemon stays firewalled: nobody answers its firewall
check. The fake node hands out one type 3 source whose buddy and whose Kad endpoint are UDP sockets
of this test. With no ed2k server, the three server paths have nothing to offer, so the daemon sends
the buddy an OP_REASKCALLBACKUDP (0x94) carrying an OP_RENDEZVOUS (0xA0) and punches the source.

Five cases: a silent source gets three requests 5 s apart and one fallback dial, and its attempt
reads as ended over EC once it is over; a source that punches back ends the requests and gets no
fallback dial; a stale record of the source that comes last does not stop the punches to its current
endpoint, and both endpoints get a fallback dial; a source that answers OP_FILENOTFOUND does not
speak the rendezvous, and stays a source; an endpoint hint (0xAA) from the source's buddy is punched
and dialled with the record's endpoint, and one from any other socket is not taken
(ModderMule/emule-qt#281).
"""
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, integer, retry_port_collisions, string, wait_for
from KadBuddyLifecycleIntegrationTest import buddy_counters
from KadSourceBuddyIdIntegrationTest import (
    FILE_HASH,
    FILE_SIZE,
    NODE_VERSION,
    SOURCE_ID,
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
OP_NATT_ENDPOINT_HINT = 0xAA
NATT_UTP, NATT_QUIC, ENDPOINT_HINT_BIT = 0x80, 0x40, 0x20
HINT_UTP, HINT_QUIC = 1, 2
# <buddy id 16><zero 16><0xA0 1><our hash 16><opts 1><file hash 16><our ip 4><udp port 2><hint 1>
ENVELOPE_SIZE = 16 + 16 + 1 + 16 + 1 + 16 + 4 + 2 + 1
SENDS = 3
SEND_INTERVAL = 5
# kPortSweepHalfWidth: a punch also sweeps this many ports on each side of the endpoint.
SWEEP_HALF_WIDTH = 16
# The daemon listens on loopback. A socket bound to the private address still reaches it from that
# address, as the fake node's answers do, so the daemon sees the source's own endpoint.
DAEMON_ADDRESS = '127.0.0.1'
DS_LOWTOLOWIP = 10
# EC_TAG_CLIENT_CONTACT_ROUTE: the route in the low nibble, 0x80 once its attempt is over.
ROUTE_KAD_BUDDY, ROUTE_MASK, ROUTE_ENDED = 4, 0x0F, 0x80
ATTEMPT = 45


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


def far_endpoint(address, others):
    """An Endpoint whose port the sweep around a punch to any of others does not reach."""
    while True:
        endpoint = Endpoint(address)
        if all(abs(endpoint.port - other.port) > SWEEP_HALF_WIDTH for other in others):
            return endpoint
        endpoint.close()


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
    assert options & ENDPOINT_HINT_BIT, ('no endpoint hint asked for', hex(options))
    assert body[50:66] == bytes.fromhex(FILE_HASH), ('file hash', body[50:66].hex())
    port, hint = struct.unpack('<HB', body[70:73])
    assert port == udp_port, ('tail port', port, udp_port)
    expected_hint = HINT_QUIC if options & NATT_QUIC else HINT_UTP
    assert hint == expected_hint, ('tail hint', hint, hex(options))


def endpoint_hint(buddy_id, endpoint):
    """An OP_NATT_ENDPOINT_HINT naming the source at endpoint, for the buddy ID the request carried."""
    return (bytes([OP_EMULEPROT, OP_NATT_ENDPOINT_HINT, 1]) + SOURCE_ID + buddy_id + bytes.fromhex(FILE_HASH)
            + socket.inet_aton(endpoint.address) + struct.pack('<HB', endpoint.port, NATT_UTP))


def source_count(ec):
    opcode, tags = ec.call_list(C['EC_OP_GET_DLOAD_QUEUE'])
    assert opcode == C['EC_OP_DLOAD_QUEUE'], opcode
    for name, _, children in tags:
        if name == C['EC_TAG_PARTFILE']:
            fields = {key: value for key, value, _ in children}
            return int.from_bytes(fields[C['EC_TAG_PARTFILE_SOURCE_COUNT']], 'big')
    raise AssertionError(('no part file', tags))


def low_to_low_routes(ec, sources):
    """Merge one incremental EC update into sources, id -> {tag: value}, and return the contact
    routes of the sources waiting LowID-to-LowID."""
    opcode, tags = ec.call_list(C['EC_OP_GET_UPDATE'], [
        integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_INC_UPDATE'])])
    assert opcode == C['EC_OP_SHARED_FILES'], opcode
    keys = (C['EC_TAG_CLIENT_DOWNLOAD_STATE'], C['EC_TAG_CLIENT_CONTACT_ROUTE'])
    for name, _, children in tags:
        if name == C['EC_TAG_CLIENT']:
            for _, client_id, fields in children:
                sources.setdefault(int.from_bytes(client_id, 'big'), {}).update(
                    {key: int.from_bytes(data, 'big') for key, data, _ in fields if key in keys})
    return [fields.get(C['EC_TAG_CLIENT_CONTACT_ROUTE']) for fields in sources.values()
            if fields.get(C['EC_TAG_CLIENT_DOWNLOAD_STATE']) == DS_LOWTOLOWIP]


def check_attempt_ends(proc, ec, sources, first_request):
    """The rendezvous reads as in flight until its attempt is over, then as ended, while the source
    stays LowID-to-LowID on the Kad buddy route."""
    deadline = first_request + ATTEMPT + 15
    while True:
        routes = low_to_low_routes(ec, sources)
        assert len(routes) == 1 and routes[0] is not None, ('no LowID-to-LowID source', sources)
        assert routes[0] & ROUTE_MASK == ROUTE_KAD_BUDDY, ('contact route', hex(routes[0]))
        if routes[0] & ROUTE_ENDED:
            break
        assert time.monotonic() < deadline, 'the rendezvous never read as ended'
        assert proc.poll() is None, 'the daemon exited'
        time.sleep(1)
    ended = time.monotonic() - first_request
    # The step went out just before its request was read.
    assert ended >= ATTEMPT - 2, ('the rendezvous read as ended early', ended)
    print(f'the rendezvous reads as ended {ended:.1f} s after its first request')


def drive(binary, address, answer, stale_record=False, hints=None, attempt_end=False):
    """One daemon and one source. answer is None, OP_HOLEPUNCH or OP_FILENOTFOUND: what the source
    sends the daemon after the first request reaches its buddy. stale_record adds an older record of
    the source last, at a port well outside the sweep around the current one. hints, a dict, has the
    buddy hint the source at a third socket and the source hint another; it gets the punches of each.
    attempt_end follows the contact route over EC until the attempt reads as ended."""
    buddy = Endpoint(address)
    source = Endpoint(address)
    # Out of each other's sweeps: a punch that reaches the decoy, or the hinted socket without the
    # hint, would be the sweep's.
    hinted = far_endpoint(address, (source,))
    decoy = far_endpoint(address, (source, hinted))
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
                first_request = buddy.received(OP_REASKCALLBACKUDP)[0][0]
                sources = {}
                if attempt_end:
                    routes = low_to_low_routes(ec, sources)
                    assert routes and routes[0] is not None and not routes[0] & ROUTE_ENDED, (
                        'the rendezvous in flight reads as ended', routes)
                if answer == OP_HOLEPUNCH:
                    source.sock.sendto(bytes([OP_EMULEPROT, OP_HOLEPUNCH]), (DAEMON_ADDRESS, udp_port))
                if hints is not None:
                    buddy_id = buddy.received(OP_REASKCALLBACKUDP)[0][1][:16]
                    source.sock.sendto(endpoint_hint(buddy_id, decoy), (DAEMON_ADDRESS, udp_port))
                    buddy.sock.sendto(endpoint_hint(buddy_id, hinted), (DAEMON_ADDRESS, udp_port))
                elif answer == OP_FILENOTFOUND:
                    before = source_count(ec)
                    assert before == 1, ('sources before the answer', before)
                    source.sock.sendto(bytes([OP_EMULEPROT, OP_FILENOTFOUND]) + bytes.fromhex(FILE_HASH),
                                       (DAEMON_ADDRESS, udp_port))
                # Past the last send and the fallback dial of the attempt. Read as it goes, since a
                # packet's time is taken when it is read.
                end = time.monotonic() + SENDS * SEND_INTERVAL + 2
                while time.monotonic() < end:
                    for endpoint in (buddy, source, hinted, decoy):
                        endpoint.drain()
                reasks = buddy.received(OP_REASKCALLBACKUDP)
                if hints is not None:
                    hints['hinted'] = hinted.received(OP_HOLEPUNCH)
                    hints['decoy'] = decoy.received(OP_HOLEPUNCH)
                punches = source.received(OP_HOLEPUNCH)
                if attempt_end:
                    check_attempt_ends(proc, ec, sources, first_request)
                    # The expiry is counted on the tick after the attempt's end.
                    wait_for(proc, 'the attempt to be counted as timed out',
                             lambda: requested(buddy_counters(ec))['timed_out'] == 1, 10)
                counters = buddy_counters(ec)
                sources = source_count(ec)
    finally:
        node.stop()
        for endpoint in (buddy, source, hinted, decoy):
            endpoint.close()
    return reasks, punches, counters, sources, udp_port


def requested(counters):
    prefix = 'OWN_REQUESTED_RENDEZVOUS_'
    return {name[len(prefix):].lower(): value for name, value in counters.items() if name.startswith(prefix)}


def run(binary):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    reasks, punches, counters, _sources, udp_port = drive(binary, address, None, attempt_end=True)
    assert len(reasks) == SENDS, ('sends to a silent source', len(reasks))
    for _, body in reasks:
        check_envelope(body, udp_port)
    gaps = [later - earlier for (earlier, _), (later, _) in zip(reasks, reasks[1:], strict=False)]
    assert all(SEND_INTERVAL - 1.5 <= gap <= SEND_INTERVAL + 2.5 for gap in gaps), ('send gaps', gaps)
    assert punches, 'the daemon never punched the source'
    assert requested(counters) == {'started': 1, 'sent': SENDS, 'punched': 0, 'fallback_dials': 1,
                                   'connected': 0, 'timed_out': 1, 'no_rendezvous': 0}, counters
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
    # The stale endpoint kept is dialled at 3 s, the real one 5 s later.
    assert requested(counters)['fallback_dials'] == 2, counters

    reasks, _punches, counters, sources, _ = drive(binary, address, OP_FILENOTFOUND)
    assert len(reasks) == 1, ('requests after OP_FILENOTFOUND', len(reasks))
    assert requested(counters) == {'started': 1, 'sent': 1, 'punched': 0, 'fallback_dials': 0,
                                   'connected': 0, 'timed_out': 0, 'no_rendezvous': 1}, counters
    assert sources == 1, ('a source that does not speak the rendezvous was dropped', sources)

    # The buddy hints where the source is now: that endpoint is punched, and the record's still is.
    hints = {}
    reasks, punches, counters, _sources, _ = drive(binary, address, None, hints=hints)
    assert hints['hinted'], 'the hinted endpoint was not punched'
    assert not hints['decoy'], ('a hint from the source itself was taken', len(hints['decoy']))
    assert punches, 'the record\'s endpoint was no longer punched'
    # The hinted endpoint is dialled at 3 s, the record's 5 s later.
    assert requested(counters)['fallback_dials'] == 2, counters
    print('PASS: a firewalled node asks a Kad source\'s buddy for a rendezvous in the sketched layout, '
          'with the buddy ID as a stock buddy compares it; a silent source gets three requests and one fallback dial, '
          'and its attempt reads as ended once it is over, '
          'a punch ends the requests, a stale record that comes last does not stop the punches to the current endpoint, '
          'OP_FILENOTFOUND ends the attempt and keeps the source, and a hint from the source\'s buddy is punched')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
