#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Issue #2335: the first and subsequent downloads must respect start-paused."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

from AllSearchIntegrationTest import (C, connect_daemon, exact, free_ports, integer, log_snapshot,
                                      retry_port_collisions, stop_daemon, stored_result,
                                      stored_search, string, tag)
from DaemonConfig import daemon_env, write_amule_conf
from ShareWatcherMoveIntegrationTest import tag_list

FIRST = bytes(range(16))
SECOND = bytes(range(16, 32))
PS_READY, PS_EMPTY, PS_PAUSED = 0, 1, 7


def request(ec, opcode, tags=()):
    # Keep repeated tags, which the general EC fixture collapses.
    payload = bytes([opcode]) + struct.pack('!H', len(tags)) + b''.join(tags)
    ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    _, length = struct.unpack('!II', exact(ec.sock, 8))
    reply = exact(ec.sock, length)
    return reply[0], tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]


def downloads(ec):
    opcode, tags = request(ec, C['EC_OP_GET_DLOAD_QUEUE'])
    assert opcode == C['EC_OP_DLOAD_QUEUE'], opcode
    result = {}
    for name, _, children in tags:
        if name == C['EC_TAG_PARTFILE']:
            fields = {key: value for key, value, _ in children}
            result[fields[C['EC_TAG_PARTFILE_HASH']]] = (
                int.from_bytes(fields[C['EC_TAG_PARTFILE_STATUS']], 'big'),
                int.from_bytes(fields[C['EC_TAG_PARTFILE_STOPPED']], 'big'))
    return result


def expect(proc, ec, expected):
    # Full allocation completes asynchronously; verify its final state too.
    deadline = time.monotonic() + 15
    while True:
        states = downloads(ec)
        if set(states) == set(expected) and all(
                state in ({PS_PAUSED} if expected[file_hash] else {PS_READY, PS_EMPTY})
                and bool(stopped) == expected[file_hash]
                for file_hash, (state, stopped) in states.items()):
            return
        assert proc.poll() is None, 'daemon exited'
        assert time.monotonic() < deadline, (expected, states)
        time.sleep(0.05)


def add(ec, source, hashes, result_ids):
    if source.startswith('link'):
        links = [string(C['EC_TAG_STRING'],
                 f'ed2k://|file|{h.hex()}.bin|4096|{h.hex()}|/') for h in hashes]
        assert ec.call(C['EC_OP_ADD_LINK'], links)[0] == C['EC_OP_NOOP']
    else:
        tags = [tag(C['EC_TAG_PARTFILE'], h, 9, children=[
                    integer(C['EC_TAG_PARTFILE_CAT'], 0),
                    *([integer(C['EC_TAG_SEARCHFILE'], result_ids[h])]
                      if source == 'search-ecid' else [])]) for h in hashes]
        assert ec.call(C['EC_OP_DOWNLOAD_SEARCH_RESULT'], tags)[0] == C['EC_OP_STRINGS']


def set_paused(ec, paused):
    assert ec.call(C['EC_OP_SET_PREFERENCES'], [
        tag(C['EC_TAG_PREFS_FILES'], children=[
            integer(C['EC_TAG_FILES_NEW_PAUSED'], int(paused))])])[0] == C['EC_OP_NOOP']


def run(binary, paused, allocate):
    with tempfile.TemporaryDirectory(prefix='amule-start-paused-') as scratch:
        root = Path(scratch)
        for directory in ('Temp', 'Incoming'):
            (root / directory).mkdir()
        port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, port, Port=peer_port, UDPEnable=0,
                         AddNewFilesPaused=int(paused), AllocateFullFile=int(allocate))
        # A real persisted search result exercises AddSearchToDownload without public peers.
        (root / 'StoredSearches.met').write_bytes(stored_search([
            stored_result('first.bin'), SECOND + stored_result('second.bin')[16:]]))
        with (root / 'console.log').open('w') as console:
            proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                    stdout=console, stderr=console, env=daemon_env(root))
            ec = None
            try:
                ec = connect_daemon(proc, port, root, peer_port)
                assert downloads(ec) == {}
                opcode, results = request(ec, C['EC_OP_SEARCH_RESULTS'],
                                          [integer(C['EC_TAG_SEARCH_ID'], 123)])
                assert opcode == C['EC_OP_SEARCH_RESULTS'], opcode
                result_ids = {}
                for name, value, children in results:
                    if name == C['EC_TAG_SEARCHFILE']:
                        fields = {key: data for key, data, _ in children}
                        result_ids[fields[C['EC_TAG_PARTFILE_HASH']]] = int.from_bytes(value, 'big')
                assert set(result_ids) == {FIRST, SECOND}, result_ids
                for source in ('link', 'search', 'search-ecid', 'link-bulk', 'search-bulk'):
                    print(f'Checking {source}: paused={paused}, full-allocation={allocate}', flush=True)
                    if source.endswith('-bulk'):
                        add(ec, source, [FIRST, SECOND], result_ids)
                    else:
                        add(ec, source, [FIRST], result_ids)
                        expect(proc, ec, {FIRST: paused})
                        add(ec, source, [SECOND], result_ids)
                    expect(proc, ec, {FIRST: paused, SECOND: paused})
                    # The preference only affects insertion: explicit resume still works.
                    assert ec.call(C['EC_OP_PARTFILE_RESUME'],
                        [tag(C['EC_TAG_PARTFILE'], FIRST, 9)])[0] == C['EC_OP_NOOP']
                    expect(proc, ec, {FIRST: False, SECOND: paused})
                    # Re-adding an existing file must preserve its explicit resumed state.
                    add(ec, source, [FIRST], result_ids)
                    expect(proc, ec, {FIRST: False, SECOND: paused})
                    for file_hash in (FIRST, SECOND):
                        assert ec.call(C['EC_OP_PARTFILE_DELETE'],
                            [tag(C['EC_TAG_PARTFILE'], file_hash, 9)])[0] == C['EC_OP_NOOP']
                    expect(proc, ec, {})

                # Remote preference edits affect future additions, preserving existing states.
                add(ec, 'link', [FIRST], result_ids)
                expect(proc, ec, {FIRST: paused})
                set_paused(ec, not paused)
                expect(proc, ec, {FIRST: paused})
                add(ec, 'search', [SECOND], result_ids)
                expected = {FIRST: paused, SECOND: not paused}
                expect(proc, ec, expected)
                ec.sock.close()
                ec = None
                stop_daemon(proc)
                snapshot = log_snapshot(root / 'logfile')
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                        stdout=console, stderr=console, env=daemon_env(root))
                ec = connect_daemon(proc, port, root, peer_port, snapshot=snapshot)
                # Loading must preserve each saved state rather than reapply the preference.
                expect(proc, ec, expected)
            finally:
                if ec is not None:
                    ec.sock.close()
                if proc.poll() is None:
                    stop_daemon(proc)
    print(f'Start-paused passed: paused={paused}, full-allocation={allocate}')


if __name__ == '__main__':
    binary = str(Path(sys.argv[1]).resolve())
    for paused in (True, False):
        for allocate in (False, True):
            retry_port_collisions(run, binary, paused, allocate)
