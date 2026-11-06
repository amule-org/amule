#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Downloading a search result that no open search holds is refused.

amuled used to answer success and queue nothing, so a client reported a download that never
happened.
"""
from pathlib import Path
import sys
import tempfile

from AllSearchIntegrationTest import (C, free_ports, retry_port_collisions, running_daemon,
                                      stored_result, stored_search, tag, wait_for)
from CompletedFileDeleteIntegrationTest import EC
from DaemonConfig import write_amule_conf

STORED_HASH = bytes(range(16))  # search_record()'s hash
UNKNOWN_HASH = bytes(range(16, 32))


def download(ec, file_hash):
    request = tag(C['EC_TAG_PARTFILE'], file_hash, 9,
                  [tag(C['EC_TAG_PARTFILE_CAT'], b'\x00', 2)])
    return ec.call('EC_OP_DOWNLOAD_SEARCH_RESULT', [request])


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-search-result-download-') as scratch:
        root = Path(scratch).resolve()
        (root / 'StoredSearches.met').write_bytes(stored_search([stored_result('stored.bin')]))
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0)
        with running_daemon(binary, root, (ec_port, peer_port), ec_class=EC) as (proc, ec):
            op, tags = download(ec, UNKNOWN_HASH)
            assert op == C['EC_OP_FAILED'], (op, tags)
            assert any(name == C['EC_TAG_STRING'] for name, _, _ in tags), tags
            print('PASS: a hash no open search holds is refused')

            op, tags = download(ec, STORED_HASH)
            assert op == C['EC_OP_STRINGS'], (op, tags)
            wait_for(proc, 'download queued', lambda: STORED_HASH.hex() in ec.files())
            print('PASS: a restored search result still downloads')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
