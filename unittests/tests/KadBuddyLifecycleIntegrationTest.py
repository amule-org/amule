#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""An open node serves up to its slot limit, one buddy per address: extra ones lose their connection,
callbacks are limited per sender and buddy, pending offers are capped, and late requests are not
promoted.

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
from KadReceiverKeyLogIntegrationTest import KADEMLIA_FINDBUDDY_REQ, KADEMLIA_FIREWALLED2_REQ
from KadSourceTagsECTest import running_kad_daemon
from UploadRefillIntegrationTest import send, wait_packet
from UploadSlotScopeIntegrationTest import OP_HELLOANSWER

KADEMLIA_FIREWALLED_ACK_RES = 0x59
KADEMLIA_CALLBACK_REQ = 0x52
KADEMLIA_FINDBUDDY_RES = 0x5A
OP_HELLO = 0x01
OP_CALLBACK = 0x99
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

    def __init__(self, address, udp_port, peer_port, buddy_id=None, user_id=None, offered=True):
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
            kad_send(peer, KADEMLIA_FINDBUDDY_REQ, self.buddy_id + self.user_id + struct.pack('<H', self.port))
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


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-kad-buddy-') as directory:
        root = Path(directory)
        # The UDP firewall check needs a peer on a public IP, and LAN mode skips it. Enough contacts
        # to outlast the test keep LAN mode on.
        write_private_contacts(root, 8)
        with running_kad_daemon(binary, root, FilterLanIPs=0, KadMaxServedBuddies=SERVED_LIMIT,
                                MaxConnections=100) as (proc, ec, udp_port, peer_port):
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
    print(f'PASS: {SERVED_LIMIT} buddies served, one per address, the extra one dropped, callbacks limited '
          'per sender and buddy, pending offers capped, an expired request not promoted, a duplicate '
          'offer counted apart, a HighID node released, one offer per address, and each counted')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
