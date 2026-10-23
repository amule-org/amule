#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep completed rows while their shared role changes on live EC/REST connections."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from AllSearchIntegrationTest import (C, PortCollision, check_daemon_ports, free_ports,
                                      integer, retry_port_collisions, stop_daemon, string, tag)
from CompletedFileDeleteIntegrationTest import EC, FIXTURES, eventually, field, http, seed_part
from DaemonConfig import write_amule_conf


class UpdateClient(EC):
    def __init__(self, port, partial):
        self.partial = partial
        super().__init__(port)
        self.rows = {}
        self.last_files = {}

    def poll(self):
        op, tags = self.call('EC_OP_GET_UPDATE',
                            [integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_INC_UPDATE'])])
        assert op == C['EC_OP_SHARED_FILES'], (op, tags)
        current = self.rows.copy() if self.partial else {}
        self.last_files = {}
        for name, ecid, children in tags:
            if name == C['EC_TAG_FILE_REMOVED']:
                current.pop(ecid, None)
            elif name in (C['EC_TAG_PARTFILE'], C['EC_TAG_KNOWNFILE']):
                previous = self.rows.get(ecid, {}).copy()
                previous.update({key: (key, value, nested) for key, value, nested in children})
                current[ecid] = previous
                self.last_files[ecid] = children
        self.rows = current
        return {field(list(children.values()), 'EC_TAG_PARTFILE_HASH').hex():
                (ecid, list(children.values())) for ecid, children in current.items()}

    def shared(self, file_hash, expected):
        rows = self.poll()
        assert file_hash in rows, ('completed transfer row disappeared', rows)
        ecid, children = rows[file_hash]
        assert field(children, 'EC_TAG_PARTFILE_STATUS') == 9, children
        return field(children, 'EC_TAG_PARTFILE_SHARED') == expected


def run(daemon, api, root):
    incoming = root / 'Incoming'
    incoming.mkdir()
    (root / 'Temp').mkdir()
    content, old_hash = FIXTURES[0]
    replacement, new_hash = FIXTURES[1]
    seed_part(root, 1, content, old_hash)
    ec_port, peer_port, udp_port, http_port = free_ports(4, udp_indices=(2,), peer_index=1)
    write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port)
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
    env.pop('LD_PRELOAD', None)
    processes, clients = [], []
    with (root / 'console.log').open('w') as log:
        try:
            core = subprocess.Popen([daemon, '-c', str(root), '--disable-fatal'],
                                    env=env, stdout=log, stderr=log)
            processes.append(core)

            def connect(partial):
                assert core.poll() is None, 'daemon exited'

                logfile = root / 'logfile'
                startup = logfile.read_text(errors='replace') if logfile.exists() else ''
                if 'Could not listen for external connections at ' in startup:
                    raise PortCollision('daemon could not bind the fixture EC port')
                if 'amuled: OnInit - starting timer' not in startup:
                    return None
                check_daemon_ports(root, ec_port, peer_port, udp_port, log_text=startup)
                try:
                    return UpdateClient(ec_port, partial)
                except ConnectionRefusedError:
                    return None

            for partial in (False, True):
                client = eventually(lambda: connect(partial))
                clients.append(client)
                def complete():
                    rows = client.poll()
                    return (old_hash in rows and
                            field(rows[old_hash][1], 'EC_TAG_PARTFILE_STATUS') == 9 and
                            field(rows[old_hash][1], 'EC_TAG_PARTFILE_SHARED') == 1)
                eventually(complete)

            api_dir = root / 'api'
            api_dir.mkdir()
            subprocess.run([api, '--config-dir='+str(api_dir), '--set-admin-pass=adminpass'],
                           env=env, check=True, stdout=log, stderr=log)
            config = api_dir / 'amuleapi.conf'
            config.write_text(config.read_text().replace('PasswordMD5=', 'PasswordMD5=' + hashlib.md5(b'regression').hexdigest(), 1))
            server = subprocess.Popen([api, '--config-dir='+str(api_dir), '--host=127.0.0.1',
                '--port='+str(ec_port), '--http-port='+str(http_port)],
                env=env, stdout=log, stderr=log)
            processes.append(server)
            base = f'http://127.0.0.1:{http_port}/api/v1'

            def healthy():
                assert server.poll() is None, 'API exited'
                try:
                    return http(base, '/health')[1].get('ec_connected')
                except OSError:
                    return False
            eventually(healthy)
            token = http(base, '/auth/login?include_token=true', method='POST',
                         body={'password': 'adminpass'})[1]['token']
            eventually(lambda: http(base, '/shared/'+old_hash, token)[0] == 200)
            finished = incoming / 'completed-1.txt'
            original_stat = finished.stat()
            identities = [client.poll()[old_hash][0] for client in clients]

            # A download-list mutation forces reconciliation without touching the
            # completed file itself. Its shared role must not oscillate false/true.
            for client in clients:
                client.poll()
                client.poll()
            active_hash = '0123456789abcdef0123456789abcdef'
            link = f'ed2k://|file|active.txt|100|{active_hash}|/'
            assert clients[0].call('EC_OP_ADD_LINK', [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
            for client, ecid in zip(clients, identities):
                client.poll()
                if client.partial:
                    assert ecid not in client.last_files, client.last_files
                else:
                    assert client.last_files[ecid] == [], client.last_files
            print('PASS: unchanged shared downloads stay quiet during reconciliation')

            # The replacement has a different hash. The old ECID remains alive in
            # downloads, so FILE_REMOVED cannot convey the loss of its shared role.
            finished.write_bytes(replacement)
            assert clients[0].call('EC_OP_SHAREDFILES_RELOAD')[0] == C['EC_OP_NOOP']
            for client in clients:
                eventually(lambda: client.shared(old_hash, 0))
            eventually(lambda: http(base, '/shared/'+old_hash, token)[0] == 404)
            eventually(lambda: http(base, '/shared/'+new_hash, token)[0] == 200)
            assert http(base, '/downloads/'+old_hash, token)[0] == 200
            print('PASS: replacement drops the old shared hash and retains its completed row')

            # Restore the original file identity so the existing completed encoder
            # becomes shared again; neither EC client nor the API is restarted.
            finished.write_bytes(content)
            os.utime(finished, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
            assert clients[0].call('EC_OP_SHAREDFILES_RELOAD')[0] == C['EC_OP_NOOP']
            for client, ecid in zip(clients, identities):
                eventually(lambda: client.shared(old_hash, 1))
                assert client.poll()[old_hash][0] == ecid
            eventually(lambda: http(base, '/shared/'+old_hash, token)[0] == 200)
            eventually(lambda: http(base, '/shared/'+new_hash, token)[0] == 404)
            print('PASS: the original completed ECID can become shared again')

            finished.rename(root / 'moved-outside-incoming.txt')
            assert clients[0].call('EC_OP_SHAREDFILES_RELOAD')[0] == C['EC_OP_NOOP']
            for client, ecid in zip(clients, identities):
                eventually(lambda: client.shared(old_hash, 0))
                assert client.poll()[old_hash][0] == ecid
                client.poll()
                if client.partial:
                    assert ecid not in client.last_files, client.last_files
                else:
                    assert client.last_files[ecid] == [], client.last_files
            eventually(lambda: http(base, '/shared/'+old_hash, token)[0] == 404)
            assert http(base, '/downloads/'+old_hash, token)[0] == 200
            print('PASS: moving the file unshares it again and subsequent polls stay quiet')
        except Exception:
            print((root / 'console.log').read_text(errors='replace'), file=sys.stderr)
            if (root / 'logfile').exists():
                print((root / 'logfile').read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            for client in clients:
                client.sock.close()
            for proc in reversed(processes):
                if proc is processes[0]:
                    stop_daemon(proc)
                else:
                    proc.terminate()
                    proc.wait(timeout=15)


def main():
    daemon, api = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-shared-membership-') as tmp:
            run(daemon, api, Path(tmp))
    retry_port_collisions(attempt)


if __name__ == '__main__':
    main()
