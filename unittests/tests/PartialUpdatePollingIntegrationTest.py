#!/usr/bin/env python3
# Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
# SPDX-License-Identifier: GPL-2.0-or-later
"""The EC_DETAIL_UPDATE polling paths for a client that negotiated partial updates.

Unfiltered GET_SHARED_FILES and GET_DLOAD_QUEUE polls omit unchanged files and send
EC_TAG_FILE_REMOVED for files that disappear. A poll that names ECIDs leaves both removal
tracking and the generation threshold intact. Search-result polls report removed results
when the search closes. The fixture uses local shared files, ed2k links, and a stored search.
"""
from contextlib import closing
import os
from pathlib import Path
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
REPLY_OPS = {'EC_OP_GET_SHARED_FILES': 'EC_OP_SHARED_FILES', 'EC_OP_GET_DLOAD_QUEUE': 'EC_OP_DLOAD_QUEUE',
             'EC_OP_SEARCH_RESULTS': 'EC_OP_SEARCH_RESULTS'}
PR_HIGH = 2


class PollingEC(EC):
    """A single-search client that asked for partial updates."""

    def __init__(self, port, **kwargs):
        super().__init__(port, multi_search=False,
                         capabilities=[tag(C['EC_TAG_CAN_PARTIAL_UPDATE'])], **kwargs)
        assert C['EC_TAG_CAN_PARTIAL_UPDATE'] in self.auth_tags, self.auth_tags


def poll(ec, op, detail, ecid=None):
    """(item ECIDs sent, ECIDs reported removed) for one request at the given detail level."""
    query = [integer(C['EC_TAG_DETAIL_LEVEL'], C[detail])]
    if ecid is not None:
        query.append(integer(C[ITEM_TAGS[op]], ecid))
    opcode, tags = ec.call_list(C[op], query)
    assert opcode == C[REPLY_OPS[op]], (op, opcode, tags)
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


def raise_priority(ec, op, file_hash):
    """A priority change gives the file a new EC generation."""
    prio = integer(C['EC_TAG_PARTFILE_PRIO'], PR_HIGH)
    assert ec.call(C[op], [tag(C['EC_TAG_PARTFILE'], file_hash, 9, [prio])])[0] == C['EC_OP_NOOP']


def check_skip_and_tombstone(proc, ec, legacy, op, touch, remove, survivor_count):
    full, removed = poll(ec, op, 'EC_DETAIL_UPDATE')
    assert len(full) == survivor_count + 1 and not removed, (op, full, removed)
    selected = min(full)
    assert poll(ec, op, 'EC_DETAIL_FULL', selected) == ({selected}, set()), op
    for detail in ('EC_DETAIL_CMD', 'EC_DETAIL_FULL', 'EC_DETAIL_UPDATE'):
        assert poll(legacy, op, detail) == (full, set()), (op, detail)
    assert poll(legacy, op, 'EC_DETAIL_UPDATE', selected) == ({selected}, set()), op
    settled(proc, ec, op)
    changed = touch()
    gone = remove()
    survivors = full - {gone}

    wait_for(proc, f'{op} full list to reflect the removal',
             lambda: poll(legacy, op, 'EC_DETAIL_FULL') == (survivors, set()), timeout=30)
    # Query the removed ECID before polling the whole list. This must not report unrelated
    # survivors as removed, consume the pending removal, or skip the survivor changed above.
    assert poll(ec, op, 'EC_DETAIL_UPDATE', gone) == (set(), set()), op
    items, removed = poll(ec, op, 'EC_DETAIL_UPDATE')
    assert changed in items and items <= survivors and removed == {gone}, (op, items, removed, gone)
    # Reported once: the next poll no longer knows the item.
    assert gone not in poll(ec, op, 'EC_DETAIL_UPDATE')[1], op
    # The full-detail control: every item, no tombstones, whatever this connection was sent.
    items, removed = poll(ec, op, 'EC_DETAIL_FULL')
    assert items == survivors and not removed, (op, items, removed)
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
            shared_hashes = {ecid: h for h, ecid in ecids(ec, 'EC_OP_GET_SHARED_FILES', 'EC_TAG_KNOWNFILE',
                                                          'EC_TAG_PARTFILE_HASH').items()}
            drop = incoming / 'drop.bin'
            drop_stat = drop.stat()

            def touch_keep():
                keep = shared_ids[b'keep.bin\0']
                raise_priority(ec, 'EC_OP_SHARED_SET_PRIO', shared_hashes[keep])
                return keep

            def unshare():
                drop.unlink()
                assert ec.call(C['EC_OP_SHAREDFILES_RELOAD'])[0] == C['EC_OP_NOOP']
                return shared_ids[b'drop.bin\0']
            legacy = EC(port, multi_search=False)
            with closing(legacy.sock):
                check_skip_and_tombstone(proc, ec, legacy, 'EC_OP_GET_SHARED_FILES', touch_keep, unshare, 1)

            # Same name, size and mtime: the reload shares the same record again, with its old ECID
            # and EC generation. The client was told it was removed, so it must be sent again.
            # Staged outside Incoming so the watcher never sees the file with a new mtime.
            staged = root / 'drop.staged'
            staged.write_bytes(b'drop.bin' * 512)
            os.utime(staged, ns=(drop_stat.st_atime_ns, drop_stat.st_mtime_ns))
            os.replace(staged, drop)
            assert ec.call(C['EC_OP_SHAREDFILES_RELOAD'])[0] == C['EC_OP_NOOP']
            drop_id = shared_ids[b'drop.bin\0']
            restored = wait_for(proc, 'drop.bin to be shared again', lambda: ecids(
                ec, 'EC_OP_GET_SHARED_FILES', 'EC_TAG_KNOWNFILE', 'EC_TAG_PARTFILE_NAME').get(b'drop.bin\0'),
                timeout=30)
            assert restored == drop_id, (restored, drop_id)
            assert drop_id in poll(ec, 'EC_OP_GET_SHARED_FILES', 'EC_DETAIL_UPDATE')[0], drop_id
            print('PASS: EC_OP_GET_SHARED_FILES: a file shared again after its removal is sent again')

            links = [string(C['EC_TAG_STRING'], f'ed2k://|file|{h.hex()}.bin|4096|{h.hex()}|/')
                     for h in DOWNLOADS]
            assert ec.call(C['EC_OP_ADD_LINK'], links)[0] == C['EC_OP_NOOP']

            def queued():
                found = ecids(ec, 'EC_OP_GET_DLOAD_QUEUE', 'EC_TAG_PARTFILE', 'EC_TAG_PARTFILE_HASH')
                return found if len(found) == len(DOWNLOADS) else None
            download_ids = wait_for(proc, 'both downloads to be queued', queued)

            def touch_kept_download():
                raise_priority(ec, 'EC_OP_PARTFILE_PRIO_SET', DOWNLOADS[0])
                return download_ids[DOWNLOADS[0]]

            def cancel():
                assert ec.call(C['EC_OP_PARTFILE_DELETE'],
                               [tag(C['EC_TAG_PARTFILE'], DOWNLOADS[1], 9)])[0] == C['EC_OP_NOOP']
                return download_ids[DOWNLOADS[1]]
            legacy = EC(port, multi_search=False)
            with closing(legacy.sock):
                check_skip_and_tombstone(proc, ec, legacy, 'EC_OP_GET_DLOAD_QUEUE', touch_kept_download,
                                         cancel, 1)

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
