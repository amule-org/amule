#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A client that closes in an upload slot logs U:0, not the US_NONE its removal from the slot sets."""
import argparse
import os
from pathlib import Path
import re
import tempfile
import time

from AllSearchIntegrationTest import free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared
from UploadSlotScopeIntegrationTest import OP_ACCEPTUPLOADREQ, Peer

PEER_NAME = 'slot-log-peer'


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-upload-slot-log-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        (root / 'Incoming' / 'shared.bin').write_bytes(os.urandom(1 << 20))
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0, MaxUpload=0,
                         Address='127.0.0.1', VerboseDebug=1, VerboseDebugLogfile=1,
                         sections={'Debug': {'Cat_ED2k Client': 1}})
        with running_daemon(binary, root, (ec_port, peer_port)) as (proc, ec):
            deadline = time.monotonic() + 30
            while 'shared.bin' not in (files := shared(ec)):
                assert time.monotonic() < deadline, 'shared file was not hashed'
                time.sleep(0.1)
            peer = Peer('127.0.0.1', '127.0.0.1', peer_port, PEER_NAME, files['shared.bin'])
            try:
                assert peer.wait_for(OP_ACCEPTUPLOADREQ, 10), 'the peer got no slot'
            finally:
                peer.close()
            pattern = re.compile(rf'client D:\d+ U:(\d+) "[^"]*{PEER_NAME}.*Reason was Close')
            deadline = time.monotonic() + 10
            while not (match := pattern.search((root / 'logfile').read_text(errors='replace'))):
                assert time.monotonic() < deadline, 'the disconnect was not logged'
                time.sleep(0.1)
            assert match[1] == '0', f'an active upload logged U:{match[1]}'
    print('a client that closed in an upload slot logged U:0')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    args = parser.parse_args()
    retry_port_collisions(run, str(Path(args.binary).resolve()))
