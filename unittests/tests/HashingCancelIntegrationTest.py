#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Refuse to cancel a download while its part file is being hashed.

The hashing task keeps a pointer to the part file, so a cancel must wait until it
finishes. A large sparse .part whose recorded date differs from its mtime is rehashed
on load, which holds it in the waiting/hashing states long enough to probe every path.
"""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from AllSearchIntegrationTest import (C, free_ports, integer, retry_port_collisions, start_daemon,
                                      stop_daemon, tag)
from CancelCompletedIntegrationTest import md4
from CompletedFileDeleteIntegrationTest import EC, eventually, field, http
from DaemonConfig import daemon_env, write_amule_conf

PARTSIZE = 9728000
SIZE = 400 * PARTSIZE - 1000  # 400 parts, below 4 GiB so the size fits a 32-bit tag
# The load needs a full hashset before it checks the date. The values only have to
# be consistent: the rehash replaces them.
PART_HASHES = [bytes([i % 256]) * 16 for i in range(400)]
FILE_HASH = md4(b''.join(PART_HASHES)).hex()
PS_WAITING_FOR_HASH, PS_HASHING = 2, 3


def seed_rehash(root):
    part = root / 'Temp' / '001.part'
    with part.open('wb') as data:
        data.truncate(SIZE)
    name = b'hashing.bin'
    tags = [b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name,
            b'\x03\x01\x00\x02' + struct.pack('<I', SIZE)]
    # One gap keeps the download incomplete after the rehash.
    for mark, value in ((0x09, SIZE - 1000), (0x0a, SIZE)):
        key = bytes([mark]) + b'0'
        tags.append(b'\x03' + struct.pack('<H', len(key)) + key + struct.pack('<I', value))
    # A recorded date older than the mtime makes the load rehash the file.
    part.with_suffix('.part.met').write_bytes(
        b'\xe0' + struct.pack('<I', int(part.stat().st_mtime) - 1000) + bytes.fromhex(FILE_HASH)
        + struct.pack('<H', len(PART_HASHES)) + b''.join(PART_HASHES)
        + struct.pack('<I', len(tags)) + b''.join(tags))
    return part


def run(daemon, api):
    with tempfile.TemporaryDirectory(prefix='amule-hashing-cancel-') as tmp:
        root = Path(tmp)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        part = seed_rehash(root)
        ec_port, peer_port, udp_port, hp = free_ports(4, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port)
        env = daemon_env(root)
        processes = []
        ec = None
        with (root / 'api.log').open('w') as api_log:
            try:
                api_root = root / 'api'
                api_root.mkdir()
                subprocess.run([api, '--config-dir=' + str(api_root), '--set-admin-pass=adminpass'],
                               env=env, check=True, stdout=api_log, stderr=api_log)
                config = api_root / 'amuleapi.conf'
                config.write_text(config.read_text().replace(
                    'PasswordMD5=', 'PasswordMD5=' + hashlib.md5(b'regression').hexdigest(), 1))
                base = f'http://127.0.0.1:{hp}/api/v1'
                proc, ec = start_daemon(daemon, root, (ec_port, peer_port, udp_port), args=(), env=env,
                                        console='daemon.log', ec_class=EC)
                processes.append(proc)
                # Start amuleapi now, not after the EC checks: the rehash takes only a few
                # seconds, and amuleapi does not retry its first connection.
                api_proc = subprocess.Popen([api, '--config-dir=' + str(api_root), '--host=127.0.0.1',
                    '--port=' + str(ec_port), '--http-port=' + str(hp)],
                    env=env, stdout=api_log, stderr=api_log)
                processes.append(api_proc)

                def status():
                    row = ec.files().get(FILE_HASH)
                    return row and field(row[1], 'EC_TAG_PARTFILE_STATUS')

                def hashing():
                    return status() in (PS_WAITING_FOR_HASH, PS_HASHING)
                assert eventually(hashing), 'the part file was not rehashed on load'
                ecid = ec.files()[FILE_HASH][0]
                legacy = tag(C['EC_TAG_PARTFILE'], bytes.fromhex(FILE_HASH), 9)
                guarded = tag(C['EC_TAG_PARTFILE'], bytes.fromhex(FILE_HASH), 9,
                              [integer(C['EC_TAG_ECID'], ecid)])
                assert ec.call('EC_OP_PARTFILE_DELETE', [legacy])[0] == C['EC_OP_FAILED'], \
                    'a client without an ECID cancelled a hashing download'
                assert ec.call('EC_OP_PARTFILE_DELETE', [guarded])[0] == C['EC_OP_FAILED']
                assert hashing() and part.exists()
                print('PASS: EC refuses to cancel a hashing download, with or without an ECID')


                def healthy():
                    assert api_proc.poll() is None, (root / 'api.log').read_text()
                    try:
                        return http(base, '/health')[1].get('ec_connected')
                    except OSError:
                        return False
                eventually(healthy)
                admin = http(base, '/auth/login?include_token=true', method='POST',
                             body={'password': 'adminpass'})[1]['token']

                def api_status():
                    code, body = http(base, '/downloads/' + FILE_HASH, admin)
                    return code == 200 and body['status']
                assert eventually(api_status) in ('waiting', 'hashing'), 'hashing finished too early'
                code, body = http(base, '/downloads/' + FILE_HASH, admin, 'DELETE')
                assert code == 409 and body['error']['code'] == 'download_hashing', (code, body)
                code, body = http(base, '/downloads', admin, 'DELETE', {'hashes': [FILE_HASH]})
                assert code == 207 and body['results'][0]['error']['code'] == 'download_hashing', (code, body)
                assert hashing() and part.exists()
                print('PASS: amuleapi answers 409 download_hashing for single and bulk cancel')

                eventually(lambda: status() not in (None, PS_WAITING_FOR_HASH, PS_HASHING), timeout=240)
                eventually(lambda: api_status() not in ('waiting', 'hashing'))
                assert http(base, '/downloads/' + FILE_HASH, admin, 'DELETE')[0] == 204
                assert not part.exists()
                print('PASS: the same download cancels once hashing has finished')
            except Exception:
                for name in ('daemon.log', 'logfile'):
                    if (root / name).exists():
                        print((root / name).read_text(errors='replace')[-8000:], file=sys.stderr)
                print((root / 'api.log').read_text(), file=sys.stderr)
                raise
            finally:
                if ec:
                    ec.sock.close()
                for child in reversed(processes):
                    stop_daemon(child, timeout=60, check=False)
    return 0


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, *sys.argv[1:]))
