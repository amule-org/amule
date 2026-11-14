#!/usr/bin/env python3
# Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
# SPDX-License-Identifier: GPL-2.0-or-later
"""The EC_DETAIL_UPDATE polling paths for a client that negotiated partial updates.

GET_SHARED_FILES and GET_DLOAD_QUEUE leave out files that did not change, and every path sends
EC_TAG_FILE_REMOVED for an item that went away. Such a client reads a missing item as "no
change", so without the tombstone a removed item would stay in its list. Offline: shared files
in Incoming, a download from an ed2k link and a stored search supply the items.
"""
from pathlib import Path
import struct
import sys
import tempfile

from AllSearchIntegrationTest import (C, EC, free_ports, integer, retry_port_collisions,
                                      running_daemon, stored_result, stored_search, string, tag,
                                      wait_for)
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import wait_shared

LEGACY_SEARCH = 0xffffffff
DOWNLOADS = [bytes([n]) * 16 for n in (0xa1, 0xa2)]
ITEM_TAGS = {'EC_OP_GET_SHARED_FILES': 'EC_TAG_KNOWNFILE', 'EC_OP_GET_DLOAD_QUEUE': 'EC_TAG_PARTFILE',
             'EC_OP_SEARCH_RESULTS': 'EC_TAG_SEARCHFILE'}


class PollingEC(EC):
    """A single-search client that asked for partial updates."""

    def __init__(self, port, **kwargs):
        super().__init__(port, multi_search=False,
                         capabilities=[tag(C['EC_TAG_CAN_PARTIAL_UPDATE'])], **kwargs)
        assert C['EC_TAG_CAN_PARTIAL_UPDATE'] in self.auth_tags, self.auth_tags


def poll(ec, op, detail):
    """(item ECIDs sent, ECIDs reported removed) for one request at the given detail level."""
    opcode, tags = ec.call_list(C[op], [integer(C['EC_TAG_DETAIL_LEVEL'], C[detail])])
    assert opcode != C['EC_OP_FAILED'], (op, tags)
    items = {int.from_bytes(value, 'big') for name, value, _ in tags if name == C[ITEM_TAGS[op]]}
    removed = {int.from_bytes(value, 'big') for name, value, _ in tags
               if name == C['EC_TAG_FILE_REMOVED']}
    return items, removed


def settled(proc, ec, op):
    """Poll until a poll sends nothing: every item is then unchanged since the last one."""
    return wait_for(proc, f'{op} to stop resending unchanged items',
                    lambda: poll(ec, op, 'EC_DETAIL_UPDATE') == (set(), set()), timeout=30)


def ecids(ec, op, item, key):
    """Map each item's key field to its ECID, from a full-detail request."""
    _, tags = ec.call_list(C[op])
    found = {}
    for name, value, children in tags:
        if name == C[item]:
            fields = {child: data for child, data, _ in children}
            found[fields[C[key]]] = int.from_bytes(value, 'big')
    return found


def check_skip_and_tombstone(proc, ec, op, remove, survivor_count):
    full, removed = poll(ec, op, 'EC_DETAIL_UPDATE')
    assert len(full) == survivor_count + 1 and not removed, (op, full, removed)
    settled(proc, ec, op)
    gone = remove()

    def tombstoned():
        items, removed = poll(ec, op, 'EC_DETAIL_UPDATE')
        assert not items & removed, (op, items, removed)
        return removed or None
    removed = wait_for(proc, f'{op} to report the removal', tombstoned, timeout=30)
    assert removed == {gone}, (op, removed, gone)
    # Reported once: the next poll no longer knows the item.
    assert gone not in poll(ec, op, 'EC_DETAIL_UPDATE')[1], op
    # The full-detail control: every item, no tombstones, whatever this connection was sent.
    items, removed = poll(ec, op, 'EC_DETAIL_FULL')
    assert len(items) == survivor_count and gone not in items and not removed, (op, items, removed)
    print(f'PASS: {op}: unchanged items left out, removal reported once')


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-partial-update-') as scratch:
        root = Path(scratch)
        incoming = root / 'Incoming'
        incoming.mkdir()
        names = ['keep.bin', 'drop.bin']
        for name in names:
            (incoming / name).write_bytes(name.encode() * 512)
        (root / 'StoredSearches.met').write_bytes(stored_search(
            [stored_result('first.bin'), bytes(range(16, 32)) + stored_result('second.bin')[16:]],
            search_id=LEGACY_SEARCH))
        port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, port, Port=peer_port, UDPEnable=0)
        with running_daemon(binary, root, (port, peer_port), ec_class=PollingEC) as (proc, ec):
            wait_shared(proc, ec, names, 60)
            shared_ids = ecids(ec, 'EC_OP_GET_SHARED_FILES', 'EC_TAG_KNOWNFILE', 'EC_TAG_PARTFILE_NAME')

            def unshare():
                (incoming / 'drop.bin').unlink()
                assert ec.call(C['EC_OP_SHAREDFILES_RELOAD'])[0] == C['EC_OP_NOOP']
                return shared_ids[b'drop.bin\0']
            check_skip_and_tombstone(proc, ec, 'EC_OP_GET_SHARED_FILES', unshare, 1)

            links = [string(C['EC_TAG_STRING'], f'ed2k://|file|{h.hex()}.bin|4096|{h.hex()}|/')
                     for h in DOWNLOADS]
            assert ec.call(C['EC_OP_ADD_LINK'], links)[0] == C['EC_OP_NOOP']

            def queued():
                found = ecids(ec, 'EC_OP_GET_DLOAD_QUEUE', 'EC_TAG_PARTFILE', 'EC_TAG_PARTFILE_HASH')
                return found if len(found) == len(DOWNLOADS) else None
            download_ids = wait_for(proc, 'both downloads to be queued', queued)

            def cancel():
                assert ec.call(C['EC_OP_PARTFILE_DELETE'],
                               [tag(C['EC_TAG_PARTFILE'], DOWNLOADS[1], 9)])[0] == C['EC_OP_NOOP']
                return download_ids[DOWNLOADS[1]]
            check_skip_and_tombstone(proc, ec, 'EC_OP_GET_DLOAD_QUEUE', cancel, 1)

            # Search results have no skip: each poll resends them all, and closing the
            # search tombstones each one.
            first, removed = poll(ec, 'EC_OP_SEARCH_RESULTS', 'EC_DETAIL_UPDATE')
            assert len(first) == 2 and not removed, (first, removed)
            assert poll(ec, 'EC_OP_SEARCH_RESULTS', 'EC_DETAIL_UPDATE') == (first, set())
            # A legacy client cannot close a search, so a multi-search client does.
            closer = EC(port)
            reply = closer.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], LEGACY_SEARCH),
                                                         tag(C['EC_TAG_SEARCH_CLOSE'])])
            assert reply[0] == C['EC_OP_MISC_DATA'], reply
            closer.sock.close()
            assert poll(ec, 'EC_OP_SEARCH_RESULTS', 'EC_DETAIL_UPDATE') == (set(), first)
            assert poll(ec, 'EC_OP_SEARCH_RESULTS', 'EC_DETAIL_UPDATE') == (set(), set())
            print('PASS: EC_OP_SEARCH_RESULTS: closing the search reports each result once')
    print('Partial-update polling paths passed')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
