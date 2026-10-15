#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send category create/update requests without a path tag to an isolated daemon."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from AllSearchIntegrationTest import C, connect_daemon, free_ports, retry_port_collisions, tag
from DaemonConfig import write_amule_conf


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-category-tag-') as directory:
        root = Path(directory)
        port, udp_port, peer_port = free_ports(3, udp_indices=(1,), peer_index=2)
        incoming = root / 'Incoming'
        write_amule_conf(root, port, Port=peer_port, UDPPort=udp_port, Autoconnect=0,
                         IncomingDir=incoming)
        with (root / 'stdout.log').open('w') as log:
            env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
            proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
            try:
                ec = connect_daemon(proc, port, root, peer_port, udp_port)
                # Category 0 always exists, so the update reaches UpdateCategory.
                category = tag(C['EC_TAG_CATEGORY'], bytes([0]), 2)
                for op in ('EC_OP_UPDATE_CATEGORY', 'EC_OP_CREATE_CATEGORY'):
                    reply, tags = ec.call(C[op], [category])
                    assert proc.poll() is None, f'{op} without a path crashed amuled'
                    # A missing path fails like an invalid one: the reply names the
                    # path the category kept.
                    assert reply == C['EC_OP_FAILED'], (op, reply, tags)
                    path = tags[C['EC_TAG_CATEGORY_PATH']][0].rstrip(b'\0').decode()
                    assert Path(path).resolve() == incoming.resolve(), (op, path)
                assert ec.call(C['EC_OP_STAT_REQ'])[0] == C['EC_OP_STATS']
                ec.sock.close()
                print('PASS: category create/update without a path tag fail cleanly')
            except BaseException:
                log.flush()
                print((root / 'stdout.log').read_text(), file=sys.stderr)
                if (root / 'logfile').exists():
                    print((root / 'logfile').read_text()[-8000:], file=sys.stderr)
                raise
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
