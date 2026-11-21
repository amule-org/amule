#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A type 7 source with the pointer route is asked through the buddy its pointer names.

A LowID with no buddy publishes type 7 with `bpv` (emule-qt ipv6-spec §5.6.1): the record names no
buddy, and its pointer names the next one the LowID gets. A downloader that cannot use the record's
`ip6` looks up the pointer at once, with a source search of size 0, and asks that buddy.

pointer: the record carries `bpv` and no `ip6`, as a LowID with no IPv6 publishes it; the pointer
         is looked up before any callback request, and the buddy it names gets one request.
ip6:     the same with an `ip6` the downloader cannot use: the pointer route is taken instead.
plain:   the record has an unusable `ip6` and no `bpv`: no lookup and no request, as before.

An unusable `ip6` is in documentation space, which a downloader never dials. One fake Kad node is
the daemon's only contact and the storer of both records.
"""
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, retry_port_collisions, string, wait_for
from KadBuddyCallbackRetryIntegrationTest import SETTLE_SECONDS, SecondBuddy
from KadBuddyPointerResolveIntegrationTest import (POINTER_KEY, TAG_BUDDYPOINTER, Storer,
                                                   marked_record)
from KadLanModeIntegrationTest import write_private_contacts
from KadSourceBuddyIdIntegrationTest import (FILE_HASH, FILE_SIZE, NODE_VERSION, SOURCE_PORT,
                                             private_address, source_tags)
from KadSourceTagsECTest import KADEMLIA2_SEARCH_SOURCE_REQ, running_kad_daemon, tag as kad_tag

# The pointer is looked up as soon as the source is first asked, well before a callback socket
# through a buddy would time out (40 s).
PROMPT_SECONDS = 20


class Type7Storer(Storer):
    """Holds a type 7 file record, with `bpv` or without, and the pointer naming `live`."""

    def __init__(self, address, live, marked, with_ip6):
        super().__init__(address, live)
        self.marked = marked
        self.with_ip6 = with_ip6
        self.file_answers = []

    def handle(self, opcode, body, peer):
        if opcode == KADEMLIA2_SEARCH_SOURCE_REQ and body[:16] != POINTER_KEY:
            self.source_searches += 1
            self.file_answers.append(time.monotonic())
            tags = source_tags(False, source_type=7)
            if not self.with_ip6:
                # source_tags() always adds one for type 7.
                tags = [tag for tag in tags if b'ip6' not in tag]
            if self.marked:
                tags.append(kad_tag(TAG_BUDDYPOINTER, 0x09, 1))
            self.send_records(peer, body[:16], [tags])
        elif opcode == KADEMLIA2_SEARCH_SOURCE_REQ:
            # <target 16><startposition 2><filesize 8>
            self.pointer_searches.append((time.monotonic(), struct.unpack('<Q', body[18:26])[0]))
            address = self.sock.getsockname()[0]
            self.send_records(peer, body[:16],
                              [marked_record(address, self.live.port, with_size=False)])
        else:
            super().handle(opcode, body, peer)


def drive(binary, address, marked, with_ip6):
    """Feed one daemon the type 7 source and return the storer, with every callback request."""
    live = SecondBuddy(address, [], label='pointer')
    node = Type7Storer(address, live, marked, with_ip6)
    for thread in (node, live):
        thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='kad-pointer-type7-') as directory:
            root = Path(directory)
            write_private_contacts(root, 1, address=address, udp_port=node.port,
                                   tcp_port=SOURCE_PORT, version=NODE_VERSION)
            with running_kad_daemon(binary, root, FilterLanIPs=0) as (proc, ec, _udp, _peer):
                wait_for(proc, 'the fake node to be asked for contacts',
                         lambda: len(node.lookups) > 0, 60)
                link = f'ed2k://|file|pointer-type7.bin|{FILE_SIZE}|{FILE_HASH}|/'
                assert ec.call(C['EC_OP_ADD_LINK'],
                               [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                wait_for(proc, 'the file search to reach the storer',
                         lambda: bool(node.file_answers), 180)
                if marked:
                    wait_for(proc, 'a callback request through the buddy the pointer names',
                             lambda: bool(node.requests), PROMPT_SECONDS + 60)
                try:
                    wait_for(proc, 'a request that must not come',
                             lambda: len(node.requests) > (1 if marked else 0), SETTLE_SECONDS)
                except AssertionError:
                    pass
    finally:
        for thread in (node, live):
            thread.stop()
    return node


def run(binary, case):
    address = private_address()
    if address is None:
        print('SKIP: no private non-loopback IPv4 for the fake Kad contact')
        sys.exit(77)

    node = drive(binary, address, marked=case != 'plain', with_ip6=case != 'pointer')
    first = node.file_answers[0]
    requests = node.requests
    print('pointer searches:', ', '.join(f'{t - first:.1f} s' for t, _ in node.pointer_searches))
    print('requests:', ', '.join(f'{t - first:.1f} s {buddy}' for t, buddy, _ in requests))
    if case == 'plain':
        assert not node.pointer_searches, ('a pointer lookup for a record without bpv',
                                           node.pointer_searches)
        assert not requests, ('a callback request for a type 7 source with no route', requests)
        print('PASS: a type 7 record without bpv or a usable ip6 is not asked')
        return

    assert len(node.pointer_searches) == 1, ('one pointer lookup', node.pointer_searches)
    assert node.pointer_searches[0][1] == 0, 'the pointer search names a file size'
    assert [buddy for _, buddy, _ in requests] == ['pointer'], (
        'not one request through the buddy the pointer names', requests)
    assert node.pointer_searches[0][0] < requests[0][0], 'asked before the pointer was looked up'
    assert requests[0][0] - first < PROMPT_SECONDS + 60, (
        f'the buddy was asked {requests[0][0] - first:.1f} s after the record arrived')
    print('PASS: the type 7 source is asked through the buddy its pointer names, once')


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[2] not in ('pointer', 'ip6', 'plain'):
        sys.exit(f'usage: {sys.argv[0]} AMULED pointer|ip6|plain')
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), sys.argv[2])
