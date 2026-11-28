#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""An open node serves up to its slot limit, one buddy per address: extra ones lose their connection,
callbacks are limited per sender and buddy, pending offers are capped, and late requests are not
promoted. A relayed rendezvous is checked and rebuilt.

The buddy counters the daemon reports over EC follow each step."""
import os
import select
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, tag, wait_for
from DownloadDisplayRatesIntegrationTest import hello
from KadLanModeIntegrationTest import (KADEMLIA2_HELLO_REQ, LAN_MODE_TIMEOUT, kad2_hello, stat, wait_stat,
                                       write_private_contacts)
from KadReceiverKeyLogIntegrationTest import (KADEMLIA_FINDBUDDY_REQ, KADEMLIA_FIREWALLED2_REQ, UDP_KEY_SECRET,
                                              encrypted, verify_key)
from KadSourceBuddyIdIntegrationTest import private_address
from KadSourceTagsECTest import kad_id, running_kad_daemon
from UploadRefillIntegrationTest import send, wait_packet
from UploadSlotScopeIntegrationTest import OP_HELLOANSWER

KADEMLIA_FIREWALLED_ACK_RES = 0x59
KADEMLIA_CALLBACK_REQ = 0x52
KADEMLIA_FINDBUDDY_RES = 0x5A
OP_HELLO = 0x01
OP_CALLBACK = 0x99
OP_EMULEPROT = 0xC5
OP_REASKCALLBACKUDP = 0x94
OP_REASKCALLBACKTCP = 0x9A
OP_RENDEZVOUS = 0xA0
OP_HOLEPUNCH = 0xA1
OP_NATT_ENDPOINT_HINT = 0xAA
ENDPOINT_HINT_BIT = 0x20
NATT_UTP = 0x80
NATT_QUIC = 0x40
# KadMaxServedBuddies, below a tenth of MaxConnections.
SERVED_LIMIT = 4
BUDDY_DISCONNECTED = 0
BUDDY_CONNECTED = 2
# A hello's little-endian client ID of 7 is a LowID: we drop a buddy with a HighID.
LOW_ID_ADDRESS = '7.0.0.0'
REQUEST_TIMEOUT = 120
IDLE_TIMEOUT = 40
# The client list runs about once a second.
MARGIN = 5


def buddy_counters(ec):
    """The children of EC_TAG_KAD_BUDDY, by name without the tag prefix."""
    prefix = 'EC_TAG_KAD_BUDDY_'
    reply = ec.call(C['EC_OP_STAT_REQ'], [tag(C['EC_TAG_KAD_BUDDY'])])[1]
    names = {code: name[len(prefix):] for name, code in C.items() if name.startswith(prefix)}
    return {names[code]: value for code, (value, _) in reply[C['EC_TAG_KAD_BUDDY']][1].items()}


def wait_counter(proc, ec, name, expected):
    wait_for(proc, f'buddy counter {name} to become {expected}',
             lambda: buddy_counters(ec)[name] == expected, MARGIN)


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
    """A firewalled node that asked for a buddy. A refused request gets no answer."""

    def __init__(self, address, udp_port, peer_port, buddy_id=None, user_id=None, offered=True, natt=True):
        self.address = address
        self.peer_port = peer_port
        self.sock = socket.socket()
        self.sock.bind((address, 0))
        self.port = self.sock.getsockname()[1]
        self.buddy_id = buddy_id or os.urandom(16)
        self.user_id = user_id or os.urandom(16)
        # CUInt128 reads four little-endian words, and the hash is their big-endian bytes.
        self.user_hash = struct.pack('>4I', *struct.unpack('<4I', self.user_id))
        with kad_peer(address, udp_port) as peer:
            # The port a hint names for this node, until a refresh moves it.
            self.kad_port = peer.getsockname()[1]
            # A NAT-T node (0x80, uTP) may take any slot, so these tests fill the whole limit; a node
            # that declares none (the 34-byte form) cannot take the third of the slots kept for NAT-T.
            options = struct.pack('<B', 0x80) if natt else b''
            kad_send(peer, KADEMLIA_FINDBUDDY_REQ,
                     self.buddy_id + self.user_id + struct.pack('<H', self.port) + options)
            if offered:
                wait_kad_packet(peer, KADEMLIA_FINDBUDDY_RES)

    def send_hello(self, client_id=LOW_ID_ADDRESS):
        self.sock.connect(('127.0.0.1', self.peer_port))
        send(self.sock, OP_HELLO, hello(self.address.encode(), client_id, self.port, user_hash=self.user_hash))

    def wait_hello_answer(self):
        assert wait_packet(self.sock, OP_HELLOANSWER, 30) is not None, 'no hello answer'

    def connect(self, client_id=LOW_ID_ADDRESS):
        self.send_hello(client_id)
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


def served(ec):
    return stat(ec, 'EC_TAG_STATS_BUDDY_SERVED')


def wait_served(proc, ec, expected):
    wait_stat(proc, ec, 'EC_TAG_STATS_BUDDY_SERVED', expected, MARGIN)


def leave(proc, ec, buddies):
    before = buddy_counters(ec)['SERVING_ENDED']
    for buddy in buddies:
        buddy.sock.close()
    wait_counter(proc, ec, 'SERVING_ENDED', before + len(buddies))


def check_slots_fill(proc, ec, udp_port, peer_port):
    buddies = [Requester(f'127.0.0.{10 + n}', udp_port, peer_port) for n in range(SERVED_LIMIT)]
    for buddy in buddies:
        buddy.connect()
    wait_served(proc, ec, SERVED_LIMIT)
    assert stat(ec, 'EC_TAG_STATS_BUDDY_SERVED_LIMIT') == SERVED_LIMIT, 'served buddy limit'
    # Served buddies are not this node's own buddy.
    assert stat(ec, 'EC_TAG_STATS_BUDDY_STATUS') == BUDDY_DISCONNECTED, 'a served buddy became our own'
    counters = buddy_counters(ec)
    assert counters['SERVING_OFFERED'] == SERVED_LIMIT and counters['SERVING_STARTED'] == SERVED_LIMIT, counters

    Requester('127.0.0.20', udp_port, peer_port, offered=False)
    wait_counter(proc, ec, 'SERVING_REFUSED_FULL', 1)
    assert buddy_counters(ec)['PENDING'] == 0, 'a request was offered with every slot in use'
    return buddies


def check_one_buddy_per_address(proc, ec, udp_port, peer_port, buddies):
    leave(proc, ec, buddies[:1])
    wait_served(proc, ec, SERVED_LIMIT - 1)
    Requester(buddies[1].address, udp_port, peer_port, offered=False)
    wait_counter(proc, ec, 'SERVING_REFUSED_SAME_IP', 1)
    return buddies[1:]


def check_extra_buddy_is_dropped(proc, ec, udp_port, peer_port):
    """Two offers for the last free slot: a pending offer holds no slot, so both are made."""
    before = buddy_counters(ec)
    first = Requester('127.0.0.21', udp_port, peer_port)
    second = Requester('127.0.0.22', udp_port, peer_port)
    # Hellos in the same cycle normally both connect, and the cap is checked again for each.
    first.send_hello()
    second.send_hello()
    first.wait_hello_answer()
    second.wait_hello_answer()
    wait_served(proc, ec, SERVED_LIMIT)
    counters = buddy_counters(ec)
    assert counters['SERVING_OFFERED'] == before['SERVING_OFFERED'] + 2, counters
    assert counters['SERVING_STARTED'] == before['SERVING_STARTED'] + 1, counters
    # The other offer ends one of two ways, depending on whether its hello came first.
    ended = ('SERVING_RELEASED_EXTRA', 'SERVING_OFFERS_SUPERSEDED')
    wait_for(proc, 'the other offer to end',
             lambda: sum(buddy_counters(ec)[name] - before[name] for name in ended) == 1, MARGIN)

    wait_for(proc, 'the extra buddy to lose its idle connection',
             lambda: first.closed() or second.closed(), IDLE_TIMEOUT + MARGIN)
    assert not (first.closed() and second.closed()), 'both buddies lost their connection'
    assert served(ec) == SERVED_LIMIT, 'the served buddy left with the extra one'
    extra, buddy = (first, second) if first.closed() else (second, first)
    extra.sock.close()
    return buddy


def callback_request(caller, buddy):
    kad_send(caller, KADEMLIA_CALLBACK_REQ, buddy.buddy_id + os.urandom(16) + struct.pack('<H', 4662))


def check_callbacks_are_limited_per_sender_and_buddy(proc, ec, udp_port, buddies):
    first, second = buddies[:2]
    before = buddy_counters(ec)
    with kad_peer('127.0.0.30', udp_port) as caller, kad_peer('127.0.0.31', udp_port) as other:
        callback_request(caller, first)
        assert wait_packet(first.sock, OP_CALLBACK, MARGIN) is not None, 'the first callback was not relayed'
        callback_request(caller, first)
        wait_counter(proc, ec, 'SERVING_CALLBACKS_RATE_LIMITED', before['SERVING_CALLBACKS_RATE_LIMITED'] + 1)
        # The limit is per buddy, and per sender.
        callback_request(caller, second)
        assert wait_packet(second.sock, OP_CALLBACK, MARGIN) is not None, 'a callback for another buddy was limited'
        callback_request(other, first)
        assert wait_packet(first.sock, OP_CALLBACK, MARGIN) is not None, 'a callback from another sender was limited'
    counters = buddy_counters(ec)
    assert counters['SERVING_CALLBACKS'] == before['SERVING_CALLBACKS'] + 3, counters
    assert counters['SERVING_CALLBACKS_RATE_LIMITED'] == before['SERVING_CALLBACKS_RATE_LIMITED'] + 1, counters


def check_pending_offers_are_capped_and_expire(proc, ec, udp_port, peer_port):
    before = buddy_counters(ec)
    requests = [Requester(f'127.0.0.{40 + n}', udp_port, peer_port) for n in range(SERVED_LIMIT + 1)]
    counters = buddy_counters(ec)
    assert counters['PENDING'] == SERVED_LIMIT, counters
    assert counters['SERVING_OFFERS_EVICTED'] == before['SERVING_OFFERS_EVICTED'] + 1, counters
    time.sleep(REQUEST_TIMEOUT + MARGIN)
    counters = buddy_counters(ec)
    assert counters['PENDING'] == 0, counters
    assert counters['SERVING_OFFERS_EXPIRED'] == before['SERVING_OFFERS_EXPIRED'] + SERVED_LIMIT, counters
    late = requests[-1]
    late.connect()
    time.sleep(MARGIN)
    assert served(ec) == 0, 'an expired request became a served buddy'
    assert buddy_counters(ec)['SERVING_STARTED'] == before['SERVING_STARTED'], 'an expired request counted as served'
    for request in requests:
        request.sock.close()


def check_duplicate_offer_is_counted_apart(proc, ec, udp_port, peer_port):
    """A node that asks again from a new address links once; its other offer is a duplicate."""
    first = Requester('127.0.0.50', udp_port, peer_port)
    again = Requester('127.0.0.51', udp_port, peer_port, first.buddy_id, first.user_id)
    before = buddy_counters(ec)
    first.connect()
    wait_served(proc, ec, 1)
    wait_for(proc, 'the second offer to be dropped as a duplicate',
             lambda: buddy_counters(ec)['SERVING_OFFERS_DUPLICATE'] == before['SERVING_OFFERS_DUPLICATE'] + 1,
             MARGIN)
    counters = buddy_counters(ec)
    for name in ('SERVING_OFFERS_EXPIRED', 'SERVING_OFFERS_SUPERSEDED', 'SERVING_OFFERS_EVICTED',
                 'SERVING_RELEASED_EXTRA'):
        assert counters[name] == before[name], (name, counters)
    assert counters['PENDING'] == 0, counters
    first.sock.close()
    again.sock.close()
    wait_served(proc, ec, 0)


def check_highid_buddy_is_released(proc, ec, udp_port, peer_port):
    """A hello whose ID is the node's own address is a HighID: it is reached directly."""
    before = buddy_counters(ec)
    node = Requester('127.0.0.60', udp_port, peer_port)
    node.connect(client_id=node.address)
    wait_counter(proc, ec, 'SERVING_RELEASED_EXTRA', before['SERVING_RELEASED_EXTRA'] + 1)
    assert served(ec) == 0, 'a node with a HighID became a served buddy'
    assert buddy_counters(ec)['SERVING_STARTED'] == before['SERVING_STARTED'], 'a HighID node counted as served'
    node.sock.close()


def check_one_offer_per_address(proc, ec, udp_port, peer_port):
    """Two nodes behind one address before either connects: only the first gets an offer."""
    # The per-IP limit allows two buddy requests a minute, so each case uses its own address.
    before = buddy_counters(ec)
    first = Requester('127.0.0.61', udp_port, peer_port)
    other = Requester('127.0.0.61', udp_port, peer_port, offered=False)
    wait_counter(proc, ec, 'SERVING_REFUSED_SAME_IP', before['SERVING_REFUSED_SAME_IP'] + 1)
    assert buddy_counters(ec)['PENDING'] == before['PENDING'] + 1, buddy_counters(ec)
    # The same node may ask again, as when our offer was lost.
    lost = Requester('127.0.0.62', udp_port, peer_port)
    again = Requester('127.0.0.62', udp_port, peer_port, lost.buddy_id, lost.user_id)
    counters = buddy_counters(ec)
    assert counters['SERVING_OFFERED'] == before['SERVING_OFFERED'] + 3, counters
    assert counters['SERVING_REFUSED_SAME_IP'] == before['SERVING_REFUSED_SAME_IP'] + 1, counters
    first.connect()
    again.connect()
    wait_served(proc, ec, 2)
    for request in (first, other, lost, again):
        request.sock.close()
    wait_served(proc, ec, 0)


def check_kept_slot_is_for_natt_nodes(proc, ec, udp_port, peer_port):
    """A third of the slots is kept for NAT-T nodes: 4 slots keep 1, so a node that declares no
    transport (the 34-byte request) fills only 3, and a NAT-T node still takes the last."""
    before = buddy_counters(ec)
    legacy = [Requester(f'127.0.0.{70 + n}', udp_port, peer_port, natt=False) for n in range(SERVED_LIMIT - 1)]
    for buddy in legacy:
        buddy.connect()
    wait_served(proc, ec, SERVED_LIMIT - 1)
    Requester('127.0.0.73', udp_port, peer_port, offered=False, natt=False)
    wait_counter(proc, ec, 'SERVING_REFUSED_FULL', before['SERVING_REFUSED_FULL'] + 1)
    assert buddy_counters(ec)['PENDING'] == before['PENDING'], 'a legacy request took the kept slot'
    natt = Requester('127.0.0.74', udp_port, peer_port)
    natt.connect()
    wait_served(proc, ec, SERVED_LIMIT)
    leave(proc, ec, legacy + [natt])


def rendezvous(requester_hash, options, file_hash, tail=b''):
    """A 0xA0 (rendezvous) as eMuleAI puts it in a reask: a zero file hash marks it."""
    return bytes(16) + bytes([OP_RENDEZVOUS]) + requester_hash + bytes([options]) + file_hash + tail


def endpoint_hint(address, port, transport):
    return socket.inet_aton(address) + struct.pack('<HB', port, transport)


def check_rendezvous_is_checked_and_rebuilt(proc, ec, udp_port, peer_port):
    """A buddy drops a rendezvous whose sides declared transports and share none, and one that does
    not parse. It relays the others built from the parsed fields, after the endpoint it saw."""
    natt = Requester('127.0.0.80', udp_port, peer_port)
    legacy = Requester('127.0.0.81', udp_port, peer_port, natt=False)
    natt.connect()
    legacy.connect()
    wait_served(proc, ec, 2)
    before = buddy_counters(ec)
    requester_hash, file_hash = os.urandom(16), os.urandom(16)
    public_hint = endpoint_hint('1.2.3.4', 4672, 1)
    with kad_peer('127.0.0.90', udp_port) as requester:
        seen = socket.inet_aton(requester.getsockname()[0]) + struct.pack('<H', requester.getsockname()[1])

        def reask(buddy, body):
            requester.send(bytes([OP_EMULEPROT, OP_REASKCALLBACKUDP]) + buddy.buddy_id + body)

        # Dropped before the one relayed, so a relay of any is the first packet the node reads.
        reask(natt, rendezvous(requester_hash, NATT_QUIC, file_hash))
        # eMuleQt sets no NAT-T bit and names QUIC in the tail, which this uTP node does not serve.
        reask(natt, rendezvous(requester_hash, 0x20 | 0x01, file_hash, endpoint_hint('1.2.3.4', 4672, 2)))
        reask(natt, rendezvous(bytes(16), NATT_UTP, file_hash))
        # Of the extension block after the hint only a usable 0x01, the requester's IPv6 UDP endpoint,
        # is relayed, and as 18 bytes.
        reask(natt, rendezvous(requester_hash, NATT_UTP | NATT_QUIC, file_hash, public_hint + b'\x01\x02xy'))
        body = wait_packet(natt.sock, OP_REASKCALLBACKTCP, MARGIN)
        assert body == seen + rendezvous(requester_hash, NATT_UTP | NATT_QUIC, file_hash, public_hint), body
        udp6 = socket.inet_pton(socket.AF_INET6, '2606:4700::1111') + struct.pack('<H', 4673)
        block = b'\x7f\x01z' + b'\x01\x14' + udp6 + b'pp'
        reask(natt, rendezvous(requester_hash, NATT_UTP | NATT_QUIC, file_hash, public_hint + block))
        body = wait_packet(natt.sock, OP_REASKCALLBACKTCP, MARGIN)
        relayed = public_hint + b'\x01\x12' + udp6
        assert body == seen + rendezvous(requester_hash, NATT_UTP | NATT_QUIC, file_hash, relayed), body

        # A node that declared nothing is relayed to whatever the requester declared. A hint address
        # that is not public goes out as zeros; its transport byte is kept.
        reask(legacy, rendezvous(requester_hash, NATT_QUIC, file_hash, endpoint_hint('10.0.0.1', 4672, 2)))
        body = wait_packet(legacy.sock, OP_REASKCALLBACKTCP, MARGIN)
        assert body == seen + rendezvous(requester_hash, NATT_QUIC, file_hash, bytes(6) + b'\x02'), body

    wait_counter(proc, ec, 'SERVING_REASKS', before['SERVING_REASKS'] + 3)
    counters = buddy_counters(ec)
    assert counters['SERVING_REASKS_MALFORMED'] == before['SERVING_REASKS_MALFORMED'] + 1, counters
    leave(proc, ec, [natt, legacy])
    wait_served(proc, ec, 0)


def check_keyed_repeat_is_a_refresh(proc, ec, udp_port, peer_port):
    """A node we serve repeats its buddy request keyed to its IP: a refresh (emule-qt#281), neither
    answered nor counted as a request. An unkeyed repeat is still refused as an address we serve.

    Two nodes, one repeat each: Kad lets an address send two buddy requests a minute."""
    keyed = Requester('127.0.0.84', udp_port, peer_port)
    plain = Requester('127.0.0.85', udp_port, peer_port)
    keyed.connect()
    plain.connect()
    wait_served(proc, ec, 2)
    before = buddy_counters(ec)
    daemon_id = kad_id(ec)
    for node, wrap in ((keyed, lambda p: encrypted(p, daemon_id, verify_key(keyed.address))),
                       (plain, lambda p: p)):
        body = node.buddy_id + node.user_id + struct.pack('<HB', node.port, NATT_UTP)
        with kad_peer(node.address, udp_port) as peer:
            peer.send(wrap(bytes([0xE4, KADEMLIA_FINDBUDDY_REQ]) + body))
    # Datagrams are handled in order, so the keyed one was handled when the plain one is counted.
    wait_counter(proc, ec, 'SERVING_REFUSED_SAME_IP', before['SERVING_REFUSED_SAME_IP'] + 1)
    counters = buddy_counters(ec)
    assert counters['SERVING_REQUESTS'] == before['SERVING_REQUESTS'] + 1, counters
    leave(proc, ec, [keyed, plain])
    wait_served(proc, ec, 0)


def wait_udp(peer, opcode):
    """The body of the next eMule-protocol packet with opcode the daemon sends peer."""
    while True:
        packet = peer.recv(65535)
        if packet[:2] == bytes([OP_EMULEPROT, opcode]):
            return packet[2:]


def check_buddy_hints_where_the_served_node_is(proc, ec, udp_port, peer_port):
    """A rendezvous that asks for a hint (0x20) gets the requester a 0xAA naming the served node's
    IPv4 and the port its 0x51 came from, and an 0xA1. A keyed refresh from another port within 3 s
    gets it a second 0xAA with that port (emule-qt#281).

    The served node is on a private address: loopback never goes in a hint, and a LAN address only
    while LAN IPs are not filtered, as here."""
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for a hinted served node')
        return False
    served = Requester(address, udp_port, peer_port)
    served.connect()
    wait_served(proc, ec, 1)
    requester_hash, file_hash = os.urandom(16), os.urandom(16)
    with kad_peer('127.0.0.92', udp_port) as requester:
        requester.send(bytes([OP_EMULEPROT, OP_REASKCALLBACKUDP]) + served.buddy_id +
                       rendezvous(requester_hash, NATT_UTP | ENDPOINT_HINT_BIT, file_hash))
        assert wait_packet(served.sock, OP_REASKCALLBACKTCP, MARGIN) is not None, 'the rendezvous was not relayed'
        named = bytes([1]) + served.user_hash + served.buddy_id + file_hash + socket.inet_aton(address)
        hint = wait_udp(requester, OP_NATT_ENDPOINT_HINT)
        assert len(hint) == 56, hint.hex()
        assert hint[:53] == named and struct.unpack('<H', hint[53:55])[0] == served.kad_port, hint.hex()
        assert hint[55] & (NATT_UTP | NATT_QUIC) == NATT_UTP, hex(hint[55])
        wait_udp(requester, OP_HOLEPUNCH)

        # The node's NAT moved it: its keyed refresh comes from another port.
        body = served.buddy_id + served.user_id + struct.pack('<HB', served.port, NATT_UTP)
        with kad_peer(address, udp_port) as moved:
            moved.send(encrypted(bytes([0xE4, KADEMLIA_FINDBUDDY_REQ]) + body, kad_id(ec), verify_key(address)))
            moved_port = moved.getsockname()[1]
        hint = wait_udp(requester, OP_NATT_ENDPOINT_HINT)
        assert hint[:53] == named and struct.unpack('<H', hint[53:55])[0] == moved_port, hint.hex()
    leave(proc, ec, [served])
    wait_served(proc, ec, 0)
    return True


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-kad-buddy-') as directory:
        root = Path(directory)
        # The UDP firewall check needs a peer on a public IP, and LAN mode skips it. Enough contacts
        # to outlast the test keep LAN mode on.
        write_private_contacts(root, 8)
        # A known UDP key secret lets a fake node key its packets to the daemon.
        with running_kad_daemon(binary, root, FilterLanIPs=0, KadMaxServedBuddies=SERVED_LIMIT,
                                MaxConnections=100,
                                sections={'Obfuscation': {'CryptoKadUDPKey': UDP_KEY_SECRET}}) as (
                                    proc, ec, udp_port, peer_port):
            pass_firewall_check(udp_port)
            wait_stat(proc, ec, 'EC_TAG_STATS_KAD_IN_LAN_MODE', 1, LAN_MODE_TIMEOUT)

            buddies = check_slots_fill(proc, ec, udp_port, peer_port)
            buddies = check_one_buddy_per_address(proc, ec, udp_port, peer_port, buddies)
            buddies.append(check_extra_buddy_is_dropped(proc, ec, udp_port, peer_port))
            check_callbacks_are_limited_per_sender_and_buddy(proc, ec, udp_port, buddies)
            leave(proc, ec, buddies)
            wait_served(proc, ec, 0)
            check_pending_offers_are_capped_and_expire(proc, ec, udp_port, peer_port)
            check_duplicate_offer_is_counted_apart(proc, ec, udp_port, peer_port)
            check_highid_buddy_is_released(proc, ec, udp_port, peer_port)
            check_one_offer_per_address(proc, ec, udp_port, peer_port)
            check_kept_slot_is_for_natt_nodes(proc, ec, udp_port, peer_port)
            check_rendezvous_is_checked_and_rebuilt(proc, ec, udp_port, peer_port)
            check_keyed_repeat_is_a_refresh(proc, ec, udp_port, peer_port)
            hinted = check_buddy_hints_where_the_served_node_is(proc, ec, udp_port, peer_port)
    print(f'PASS: {SERVED_LIMIT} buddies served, one per address, the extra one dropped, callbacks limited '
          'per sender and buddy, pending offers capped, an expired request not promoted, a duplicate '
          'offer counted apart, a HighID node released, one offer per address, the kept slot only for a '
          'NAT-T node, a rendezvous checked and rebuilt, a keyed repeat taken as a refresh, '
          + ('a hint sent and sent again after a refresh, ' if hinted else '') + 'and each counted')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
