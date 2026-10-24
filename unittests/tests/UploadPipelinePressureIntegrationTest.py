#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe pipe-held storage and a non-reading receiver without changing policy."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

from AllSearchIntegrationTest import connect_daemon, free_ports, retry_port_collisions, stop_daemon
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared
from UploadStorageDelayIntegrationTest import BLOCK, Peer
from UploadStoragePoolIntegrationTest import run as storage_run, wait_pipeline


def receiver_run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-pipeline-receiver-') as directory:
        root = Path(directory)
        incoming = root / 'Incoming'
        incoming.mkdir()
        (root / 'Temp').mkdir()
        content = os.urandom(24 * 3 * BLOCK)
        (incoming / 'payload.bin').write_bytes(content)
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0,
                         MaxUpload=3500, SlotAllocation=100, MMapEnabled=0,
                         SharedFileReverifyDays=0, IncomingDir=incoming, VerboseDebug=1,
                         sections={'Debug': {'Cat_FileIO': 1}})
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        env.pop('LD_PRELOAD', None)
        peers = []
        proc = ec = None
        with (root / 'console.log').open('w') as log:
            try:
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], env=env,
                                        stdout=log, stderr=log)
                ec = connect_daemon(proc, ec_port, root, peer_port)
                deadline = time.monotonic() + 30
                while 'payload.bin' not in (files := shared(ec)):
                    assert time.monotonic() < deadline, 'shared payload missing'
                    time.sleep(0.05)
                peers.append(Peer(peer_port, 0, files['payload.bin'], receive_buffer=4096))
                slow = peers[0]
                # This peer deliberately stops reading. Its small advertised receive
                # window plus bounded queued requests produce real socket backpressure.
                slow.request([(i * 3 * BLOCK, (i + 1) * 3 * BLOCK) for i in range(24)])
                blocked = wait_pipeline(root, proc, lambda sample:
                    sample['transport_busy'] > 0 and sample['ready_bytes'] > 0
                    and sample['sustained'] == 1)
                peers.append(Peer(peer_port, 1, files['payload.bin']))
                peers[1].request([(0, BLOCK)])
                peers[1].receive((0, BLOCK), content)
                slow.close()
                wait_pipeline(root, proc, lambda sample:
                    sample['active'] > 0 and sample['active'] == sample['drained']
                    and sample['pending_reads'] == 0 and sample['transport_busy'] == 0
                    and sample['underfilled'] == 0 and sample['sustained'] == 0,
                    after=blocked['sample_ms'])
                print('pipeline: held receiver reports transport backlog; healthy peer transfers verified; recovery clears pressure')
            except BaseException:
                print((root / 'console.log').read_text()[-5000:])
                logfile = root / 'logfile'
                if logfile.exists():
                    print(logfile.read_text(errors='replace')[-6000:])
                raise
            finally:
                for peer in peers:
                    peer.close()
                if ec:
                    ec.sock.close()
                if proc:
                    stop_daemon(proc)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('fixture')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    fixture = str(Path(args.fixture).resolve())
    retry_port_collisions(storage_run, binary, fixture, 'read', False, True)
    retry_port_collisions(receiver_run, binary)
