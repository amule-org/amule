#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check plaintext migration and digest authentication against an isolated offline core."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.request

from AllSearchIntegrationTest import (PortCollision, free_ports, retry_port_collisions,
                                      start_daemon, stop_daemon)
from DaemonConfig import daemon_env, write_amule_conf


def stop(process):
    stop_daemon(process, timeout=15, check=False)


def run(daemon_binary, api_binary):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    # A 32-hex plaintext secret must remain plaintext in the legacy field.
    for secret in ('regression', 'abcdef0123456789abcdef0123456789'):
        with tempfile.TemporaryDirectory(prefix='amule-ec-password-') as temporary:
            root = Path(temporary)
            core, api = root / 'core', root / 'api'
            core.mkdir()
            api.mkdir()
            ec_port, http_port, peer_port, udp_port = free_ports(4, udp_indices=(3,), peer_index=2)
            write_amule_conf(core, ec_port, ec_password=secret, Port=peer_port,
                             UDPEnable=1, UDPPort=udp_port, sections={'WebServer': {'Enabled': 0}})
            env = daemon_env(root)
            with (root / 'fixture.log').open('w') as log:
                daemon = None
                try:
                    daemon, ec = start_daemon(daemon_binary, core, (ec_port, peer_port, udp_port),
                                              env=env, password=secret)
                    ec.sock.close()
                    command = [api_binary, '--config-dir=' + str(api)]
                    # Verify the input path stays compatible with the existing REST password derivation.
                    admin, guest = '密碼 with spaces', 'guest secret'
                    for role, password in (('admin', admin), ('guest', guest)):
                        subprocess.run(command + ['--set-' + role + '-pass-stdin'],
                                       input=(password + '\n').encode(), check=True, env=env,
                                       stdout=log, stderr=log, timeout=10)
                    for mode in ('legacy', 'digest', 'migrate'):
                        conf = api / 'amuleapi.conf'
                        credential = ('PasswordMD5=' + hashlib.md5(secret.encode()).hexdigest().upper()
                                      if mode == 'digest' else 'Password=' + secret)
                        conf.write_text('[EC]\n' + credential + '\n', encoding='utf-8')
                        conf.chmod(0o600)
                        if mode == 'migrate':
                            subprocess.run(command + ['--set-ec-pass-stdin'], input=(secret + '\n').encode(),
                                           check=True, env=env, stdout=log, stderr=log, timeout=10)
                            assert 'Password=' not in conf.read_text()
                        server = subprocess.Popen(command + ['--host=127.0.0.1', '--port=' + str(ec_port),
                            '--http-port=' + str(http_port)],
                            env=env, stdout=log, stderr=log)
                        try:
                            url = f'http://127.0.0.1:{http_port}/api/v1'
                            deadline = time.monotonic() + 30
                            while True:
                                assert server.poll() is None, 'API exited'
                                log.flush()
                                if 'HTTP server failed to start:' in (root / 'fixture.log').read_text():
                                    raise PortCollision('API port collision')
                                assert time.monotonic() < deadline, 'API did not connect to EC'
                                try:
                                    with opener.open(url + '/health', timeout=1) as response:
                                        if json.load(response).get('ec_connected'):
                                            break
                                except OSError:
                                    pass
                                time.sleep(0.1)
                            for password in (admin, guest):
                                request = urllib.request.Request(url + '/auth/login?include_token=true',
                                    data=json.dumps({'password': password}).encode(),
                                    headers={'Content-Type': 'application/json'})
                                with opener.open(request, timeout=5) as response:
                                    assert json.load(response)['token']
                            print(f'PASS: {mode} EC authentication and stdin role login (secret length {len(secret)})', flush=True)
                        finally:
                            stop(server)
                except BaseException:
                    log.flush()
                    for path in (core / 'console.log', root / 'fixture.log'):
                        if path.exists():
                            print(path.read_text(errors='replace')[-12000:], file=sys.stderr)
                    raise
                finally:
                    if daemon is not None:
                        stop(daemon)


if __name__ == '__main__':
    retry_port_collisions(run, *sys.argv[1:])
