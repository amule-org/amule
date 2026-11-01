#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep completed rows while their shared role changes on live EC/REST connections."""
import os
from pathlib import Path
import sys
import tempfile

from AllSearchIntegrationTest import (C, configure_amuleapi, connect_daemon, free_ports, http,
                                      integer, retry_port_collisions, start_amuleapi,
                                      start_daemon, stop_daemon, string, wait_amuleapi)
from CompletedFileDeleteIntegrationTest import EC, FIXTURES, eventually, field, seed_part
from DaemonConfig import daemon_env, write_amule_conf


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
    env = daemon_env(root)
    core = server = None
    clients = []
    # Append: the daemon writes the same console through its own handle.
    with (root / 'console.log').open('a') as log:
        try:
            core, client = start_daemon(daemon, root, (ec_port, peer_port, udp_port), env=env,
                                        ec_class=UpdateClient, partial=False)
            for partial in (False, True):
                if partial:
                    client = connect_daemon(core, ec_port, ec_class=UpdateClient, partial=True)
                clients.append(client)
                def complete():
                    rows = client.poll()
                    return (old_hash in rows and
                            field(rows[old_hash][1], 'EC_TAG_PARTFILE_STATUS') == 9 and
                            field(rows[old_hash][1], 'EC_TAG_PARTFILE_SHARED') == 1)
                eventually(complete)

            api_dir = root / 'api'
            api_dir.mkdir()
            configure_amuleapi(api, api_dir, env, log)
            server = start_amuleapi(api, api_dir, ec_port, http_port, env, log)
            base = f'http://127.0.0.1:{http_port}/api/v1'
            token = wait_amuleapi(server, base, log.name)
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
            if server is not None:
                stop_daemon(server, timeout=15, check=sys.exc_info()[0] is None)
            if core is not None:
                stop_daemon(core, check=sys.exc_info()[0] is None)


def main():
    daemon, api = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-shared-membership-') as tmp:
            run(daemon, api, Path(tmp))
    retry_port_collisions(attempt)


if __name__ == '__main__':
    main()
