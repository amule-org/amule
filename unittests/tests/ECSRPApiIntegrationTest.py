#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team. SPDX-License-Identifier: GPL-2.0-or-later
"""Real amuleapi password and spawned-token logins against an offline core."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from AllSearchIntegrationTest import connect_daemon, free_ports, retry_port_collisions, stop_daemon
from DaemonConfig import write_amule_conf


def run(daemon_binary, api_binary, token):
    with tempfile.TemporaryDirectory(prefix='amule-srp-api-') as temporary:
        root = Path(temporary)
        port, http, peer = free_ports(3, peer_index=2)
        sections = {'AmuleApi': {'Enabled': int(token), 'Path': api_binary,
                                'BindAddress': '127.0.0.1', 'HttpPort': http}}
        write_amule_conf(root, port, Port=peer, UDPEnable=0, sections=sections)
        # In token mode an intentionally wrong configured password proves the
        # production child uses its token rather than silently falling back.
        (root / 'amuleapi.conf').write_text(
            '[Server]\nBindAddress=127.0.0.1\nPort=' + str(http) + '\n'
            '[EC]\nHost=127.0.0.1\nPort=' + str(port) + '\nPassword=' +
            ('wrong-password' if token else 'regression') + '\nEncryption=0\n')
        (root / 'amuleapi.conf').chmod(0o600)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'),
                   LC_ALL='C.UTF-8')
        processes = []
        with (root / 'console').open('w') as log:
            try:
                daemon = subprocess.Popen([daemon_binary, '-c', str(root)] + ([] if os.name == 'nt' else ['--disable-fatal']),
                                          env=env, stdout=log, stderr=log)
                processes.append(daemon)
                ec = connect_daemon(daemon, port, root, peer)
                ec.sock.close()
                if not token:
                    processes.append(subprocess.Popen(
                        [api_binary, '--config-dir=' + str(root)],
                        env=env, stdout=log, stderr=log))
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                deadline = time.monotonic() + 30
                while True:
                    assert all(p.poll() is None for p in processes), (root / 'console').read_text()
                    try:
                        with opener.open(f'http://127.0.0.1:{http}/api/v1/health', timeout=2) as response:
                            assert response.status == 200
                            json.load(response)
                        break
                    except urllib.error.URLError:
                        assert time.monotonic() < deadline, (root / 'console').read_text()
                        time.sleep(0.05)
                api_log = (root / 'amuleapi.log').read_text()
                assert 'Succeeded!' in api_log, api_log
                assert ('using the ephemeral EC token' in api_log) == token, api_log
                assert not (root / 'amuleapi-ec-token').exists(), 'token not consumed'
                assert '\nECPassword=' not in (root / 'amule.conf').read_text()
                print('PASS: production amuleapi SRP login:', 'spawned token' if token else 'password')
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        stop_daemon(process)


if __name__ == '__main__':
    for token in (False, True):
        retry_port_collisions(lambda: run(*map(lambda p: str(Path(p).resolve()), sys.argv[1:]), token))
