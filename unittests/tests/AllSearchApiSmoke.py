#!/usr/bin/env python3
"""Run the curl All-search checks against an isolated, offline core/API pair."""
import hashlib
import re
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time

from AllSearchIntegrationTest import (C, PortCollision, connect_daemon, free_ports, integer,
                                      retry_port_collisions, start_daemon, stop_daemon,
                                      stored_search)
from DaemonConfig import daemon_env, write_amule_conf


def run(daemon_binary, api_binary, checks):
    with tempfile.TemporaryDirectory(prefix='amule-all-api-') as temporary:
        root = Path(temporary)
        core, api = root / 'core', root / 'api'
        core.mkdir()
        api.mkdir()
        port, http, peer_port, udp_port = free_ports(4, udp_indices=(3,), peer_index=2)
        write_amule_conf(core, port, Port=peer_port, UDPEnable=1, UDPPort=udp_port, Autoconnect=0,
                         ConnectToKad=1, sections={'WebServer': {'Enabled': 0}})
        (core / 'nodes.dat').write_bytes(struct.pack('<III', 0, 1, 0))
        # A finished All search exercises discovery and progress serialization
        # without public peers; later checks start Kad with no contacts.
        (core / 'StoredSearches.met').write_bytes(stored_search([]))
        env = daemon_env(root)
        processes = []
        with (root / 'fixture.log').open('w') as log:
            try:
                daemon, ec = start_daemon(daemon_binary, core, (port, peer_port, udp_port),
                                          args=(), env=env)
                processes.append(daemon)
                ec.sock.close()
                subprocess.run([api_binary, f'--config-dir={api}',
                                '--set-admin-pass=adminpass'], check=True,
                               env=env, stdout=log, stderr=log)
                config = api / 'amuleapi.conf'
                config.write_text(config.read_text().replace(
                    'PasswordMD5=', 'PasswordMD5=' + hashlib.md5(b'regression').hexdigest(), 1))
                server = subprocess.Popen([api_binary, f'--config-dir={api}',
                                           '--host=127.0.0.1', f'--port={port}',
                                           f'--http-port={http}'],
                                          env=env, stdout=log, stderr=log)
                processes.append(server)
                url = f'http://127.0.0.1:{http}/api/v1'
                # Startup can include expensive password hashing. Let the
                # caller impose a deadline, while detecting process failure.
                while True:
                    if any(proc.poll() is not None for proc in processes):
                        log.flush()
                        failure = (root / 'fixture.log').read_text(errors='replace')
                        if 'HTTP server failed to start:' in failure:
                            raise PortCollision(f'API could not bind HTTP port {http}')
                        raise RuntimeError('fixture process exited during startup')
                    log.flush()
                    api_listening = (f'listening on http://127.0.0.1:{http}/'
                                     in (root / 'fixture.log').read_text(errors='replace'))
                    if api_listening and subprocess.run(
                            ['curl', '-sf', '--max-time', '2', url + '/health'],
                            stdout=subprocess.DEVNULL).returncode == 0:
                        break
                    time.sleep(0.1)
                # Exercise the command parser and real EC_SEARCH_ALL request too.
                command = Path(daemon_binary).with_name('amulecmd')
                if command.exists():
                    ec = connect_daemon(daemon, port, core, peer_port, udp_port)
                    assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                    result = subprocess.run([
                        str(command), '-h', '127.0.0.1', '-p', str(port),
                        '-P', 'regression', '-c',
                        'search all offlinecommandregression --type Arc --avail 2'],
                        check=True, env=env, capture_output=True, text=True)
                    match = re.search(r'Search started \(id (\d+)\)', result.stdout)
                    assert match, result.stdout + result.stderr
                    sid = int(match.group(1))
                    progress = ec.progress(sid)
                    assert progress[C['EC_TAG_SEARCH_LIFECYCLE_KIND']][0] == C['EC_SEARCH_ALL']
                    assert progress[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
                    ec.call(C['EC_OP_SEARCH_STOP'], [integer(C['EC_TAG_SEARCH_ID'], sid)])
                    ec.sock.close()
                    print('PASS: amulecmd search all starts a combined search with filters')
                subprocess.run(['bash', checks, '--fixture-ready'], check=True,
                               env=dict(env, API=url, ALL_SID='123'))
            except BaseException:
                log.flush()
                for path in (core / 'console.log', root / 'fixture.log'):
                    if path.exists():
                        print(path.read_text(errors='replace')[-12000:], file=sys.stderr)
                raise
            finally:
                for proc in reversed(processes):
                    stop_daemon(proc, timeout=30, check=False)


if __name__ == '__main__':
    retry_port_collisions(run, *sys.argv[1:])
