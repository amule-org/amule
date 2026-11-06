#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A client that closes in an upload slot logs U:0, not the US_NONE its removal from the slot sets."""
import argparse
import os
from pathlib import Path
import re
import tempfile

from AllSearchIntegrationTest import free_ports, retry_port_collisions, running_daemon, wait_for
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import wait_shared
from UploadSlotScopeIntegrationTest import OP_ACCEPTUPLOADREQ, Peer

PEER_NAME = 'slot-log-peer'


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-upload-slot-log-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Incoming' / 'shared.bin').write_bytes(os.urandom(1 << 20))
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0, MaxUpload=0,
                         VerboseDebug=1, VerboseDebugLogfile=1,
                         sections={'Debug': {'Cat_ED2k Client': 1, 'Cat_FileIO': 1}})
        with running_daemon(binary, root, (ec_port, peer_port)) as (proc, ec):
            files = wait_shared(proc, ec, ['shared.bin'], 30)
            peer = Peer('127.0.0.1', '127.0.0.1', peer_port, PEER_NAME, files['shared.bin'])
            try:
                assert peer.wait_for(OP_ACCEPTUPLOADREQ, 10), 'the peer got no slot'
            finally:
                peer.close()
            pattern = re.compile(rf'client D:\d+ U:(\d+) "[^"]*{PEER_NAME}.*Reason was Close')
            match = wait_for(proc, 'the disconnect to be logged',
                             lambda: pattern.search((root / 'logfile').read_text(errors='replace')), 10)
            assert match[1] == '0', f'an active upload logged U:{match[1]}'
            log = (root / 'logfile').read_text(errors='replace')
            assert re.search(r'Upload slot ended: id=\d+ reason="Close" seconds=\d+ session_bytes=\d+', log), \
                'the slot end was not logged'
    print('a client that closed in an upload slot logged U:0')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    args = parser.parse_args()
    retry_port_collisions(run, str(Path(args.binary).resolve()))
