#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""One daemon, two real filesystems: a stalled .part cannot stop a cold healthy file."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, connect_daemon, free_ports, retry_port_collisions, stop_daemon, tag
from CancelCompletedIntegrationTest import part_met
from DaemonConfig import write_amule_conf
from ED2KLinkIntegrationTest import add_link
from WriteBackpressureIntegrationTest import Source, wait_for, wait_for_plateau, LIMIT, BLOCK


def run(binary, shim, compressed, mode):
    if not Path('/dev/shm').is_dir() or os.stat('/tmp').st_dev == os.stat('/dev/shm').st_dev:
        print('SKIP: two writable filesystem devices are required')
        return 77
    with tempfile.TemporaryDirectory(prefix='amule-write-volumes-') as directory, \
            tempfile.TemporaryDirectory(prefix='amule-healthy-volume-', dir='/dev/shm') as healthy_directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        bad_dir = root / 'stalled-volume'
        bad_dir.mkdir()
        healthy_dir = Path(healthy_directory)
        assert bad_dir.stat().st_dev != healthy_dir.stat().st_dev
        bad, healthy = [Source(index, compressed) for index in range(2)]
        sources = [bad, healthy]
        for index, (source, target_dir) in enumerate(zip(sources, (bad_dir, healthy_dir)), 1):
            target = target_dir / 'data.part'
            target.touch()
            (root / f'Temp/{index:03}.part').symlink_to(target)
            (root / f'Temp/{index:03}.part.met').write_bytes(
                part_met(source.file_hash, source.name.decode(), 67108864, [(0, 67108864)]))
        gate = root / 'stall'
        gate.touch()
        ports = free_ports(2, peer_index=1)
        write_amule_conf(root, ports[0], Address=bad.address, Port=ports[1], UDPEnable=0,
                         FilterLanIPs=0, MaxDownload=0, MaxUpload=0)
        preload = ':'.join(filter(None, [os.environ.get('AMULE_TEST_PRELOAD_RUNTIME'), shim]))
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8',
                   LD_PRELOAD=preload, AMULE_TEST_WRITE_GATE=str(gate),
                   AMULE_TEST_WRITE_PREFIX=str(bad_dir))
        if mode == 'space':
            env['AMULE_TEST_SPACE_PATH'] = str(root / 'Temp/001.part')
        proc = ec = None
        try:
            with (root / 'console.log').open('w') as log:
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], env=env,
                                        stdout=log, stderr=log)
            ec = connect_daemon(proc, ports[0], root, ports[1])
            ec.sock.settimeout(3)
            detail = lambda: ([s.requested for s in sources], [s.errors for s in sources],
                              (root / 'logfile').read_text(errors='replace')[-3000:])
            assert add_link(ec, bad.link()) == C['EC_OP_NOOP']
            bad.start(ports[1])
            wait_for(lambda: Path(str(gate) + '.entered').exists() and
                     bad.requested >= LIMIT - 2 * BLOCK, 15, detail)
            stalled = wait_for_plateau(lambda: bad.requested, LIMIT, detail)
            # This file is deliberately cold: its first discovery/request starts
            # only after the other volume is already stuck and at its byte cap.
            assert add_link(ec, healthy.link()) == C['EC_OP_NOOP']
            healthy.start(ports[1])
            wait_for(lambda: healthy.requested >= 3 * 1024 * 1024, 5, detail)
            healthy.stopped.set()
            def written():
                if healthy.first_start is None:
                    return False
                with (healthy_dir / 'data.part').open('rb') as data:
                    data.seek(healthy.first_start)
                    return data.read(32) == b'x' * 32
            wait_for(written, 3, detail)
            assert bad.requested == stalled and stalled <= LIMIT, detail()
            assert not bad.cancelled and not any(s.errors for s in sources), detail()
            ec.call(C['EC_OP_GET_DLOAD_QUEUE'])
            # Cancelling a healthy file must not join/wait for the stalled worker.
            assert ec.call(C['EC_OP_PARTFILE_DELETE'],
                           [tag(C['EC_TAG_PARTFILE'], healthy.file_hash, 9)])[0] == C['EC_OP_NOOP']
            gate.unlink()
            wait_for(lambda: bad.requested > stalled + 3 * BLOCK, 8, detail)
            print(f'PASS mode={mode} compressed={compressed}: stalled volume bounded at {stalled}; '
                  'cold healthy volume wrote data and cancelled independently; stalled volume resumed',
                  flush=True)
        finally:
            gate.unlink(missing_ok=True)
            for source in sources:
                if hasattr(source, 'worker'):
                    source.close()
                else:
                    source.identity.close()
            if ec:
                ec.sock.close()
            if proc:
                stop_daemon(proc)
    return 0


if __name__ == '__main__':
    for mode in ('write', 'space'):
        for compressed in (False, True):
            result = retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve()),
                                                      str(Path(sys.argv[2]).resolve()), compressed, mode))
            if result:
                sys.exit(result)
