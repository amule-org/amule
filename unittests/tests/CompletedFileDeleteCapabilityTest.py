#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify deletion capability gates and acknowledgments with deterministic EC fixtures."""
import hashlib
import os
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import zlib

from ECWire import accept  # Preflight in the parent before starting fixtures.
from AllSearchIntegrationTest import C, exact, free_ports, integer, string, tag
from CompletedFileDeleteIntegrationTest import eventually, http

FILE_HASH = 'a448017aaf21d8525fc10ae87aa6729d'


class LegacyCore(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            caps = [tag(0x002A)]
            if self.server.mode != 'legacy': caps.append(tag(C['EC_TAG_CAN_DELETE_COMPLETE']))
            self.request = accept(self.request, capabilities=caps)
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1:
                    body = zlib.decompress(body)
                op = body[0]
                self.server.ops.append(op)
                if op == C['EC_OP_GET_UPDATE']:
                    children = [tag(C['EC_TAG_PARTFILE_HASH'], bytes.fromhex(FILE_HASH), 9),
                                string(C['EC_TAG_PARTFILE_NAME'], 'completed.txt'),
                                integer(C['EC_TAG_PARTFILE_SIZE_FULL'], 3),
                                tag(C['EC_TAG_PARTFILE_STATUS'], b'\x08' if self.server.mode == 'completing' else b'\x09', 2),
                                string(C['EC_TAG_KNOWNFILE_PATH'], '/Incoming')]
                    reply, tags = C['EC_OP_SHARED_FILES'], [tag(C['EC_TAG_PARTFILE'], struct.pack('!I', 7), 4, children),
                        tag(C['EC_TAG_KNOWNFILE'], struct.pack('!I', 7), 4, children)]
                elif op == C['EC_OP_KNOWNFILE_DELETE']:
                    # A packet other than the deletion acknowledgment is not
                    # evidence that any file was removed.
                    reply, tags = C['EC_OP_SHARED_FILES'], []
                else:
                    reply, tags = C['EC_OP_NOOP'], []
                payload = bytes([reply]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        except (EOFError, OSError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


def run(api, mode):
    with tempfile.TemporaryDirectory(prefix='amule-delete-legacy-') as tmp:
        root = Path(tmp)
        env = dict(os.environ, HOME=tmp, XDG_CONFIG_HOME=tmp+'/xdg', LC_ALL='C.UTF-8')
        subprocess.run([api, '--config-dir='+tmp, '--set-admin-pass=adminpass'], env=env,
                       check=True, stdout=subprocess.DEVNULL)
        config = root / 'amuleapi.conf'
        config.write_text(config.read_text().replace('PasswordMD5=', 'PasswordMD5=' + hashlib.md5(b'regression').hexdigest(), 1))
        with Server(('127.0.0.1', 0), LegacyCore) as core, (root / 'api.log').open('w') as log:
            core.ops = []
            core.mode = mode
            threading.Thread(target=core.serve_forever, daemon=True).start()
            hp = free_ports(1)[0]
            base = f'http://127.0.0.1:{hp}/api/v1'
            proc = subprocess.Popen([api, '--config-dir='+tmp, '--host=127.0.0.1',
                '--port='+str(core.server_address[1]), '--http-port='+str(hp)],
                env=env, stdout=log, stderr=log)
            try:
                def healthy():
                    assert proc.poll() is None, (root / 'api.log').read_text()
                    try:
                        return http(base, '/health')[1].get('ec_connected')
                    except OSError:
                        return False
                eventually(healthy)
                token = http(base, '/auth/login?include_token=true', method='POST', body={'password':'adminpass'})[1]['token']
                eventually(lambda: http(base, '/downloads/'+FILE_HASH, token)[0] == 200)
                expected_status, expected_bulk, expected_code = {
                    'legacy': (503, 503, 'ec_unsupported'),
                    'completing': (409, 207, 'download_completed'),
                    'wrong_ack': (502, 207, 'ec_protocol_error'),
                }[mode]
                detail = http(base, '/downloads/'+FILE_HASH, token)[1]
                target = {k: detail[k] for k in ('ecid', 'name', 'directory', 'status')}
                status, result = http(base, '/downloads/'+FILE_HASH, token, 'DELETE', {'confirmed_target': target})
                assert status == expected_status and result['error']['code'] == expected_code, (status, result)
                status, result = http(base, '/downloads', token, 'DELETE', {'hashes':[FILE_HASH], 'confirmed_target': {FILE_HASH: target}})
                assert status == expected_bulk and result['results'][0]['error']['code'] == expected_code, (status, result)
                shared = http(base, '/shared/'+FILE_HASH, token)[1]
                rows = http(base, '/shared', token)[1]['shared']
                assert len(rows) == 1 and rows[0].get('incomplete') is (mode == 'completing'), rows
                assert rows[0]['incomplete'] == shared['incomplete']
                target = {k: shared[k] for k in ('ecid', 'name', 'directory', 'incomplete')}
                status, result = http(base, '/shared/'+FILE_HASH, token, 'DELETE', {'confirmed_target': target})
                shared_status, shared_code = (409, 'partfile_unsupported') if mode == 'completing' else (expected_status, expected_code)
                assert status == shared_status and result['error']['code'] == shared_code, (status, result)
                if mode == 'wrong_ack':
                    assert core.ops.count(C['EC_OP_KNOWNFILE_DELETE']) == 3, core.ops
                else:
                    assert C['EC_OP_KNOWNFILE_DELETE'] not in core.ops, core.ops
                assert C['EC_OP_PARTFILE_DELETE'] not in core.ops, core.ops
                print('PASS:', mode, 'single and bulk deletion return', expected_code)
            except Exception:
                print((root / 'api.log').read_text(), file=sys.stderr)
                raise
            finally:
                proc.terminate()
                proc.wait(timeout=15)
                core.shutdown()


if __name__ == '__main__':
    for mode in ('legacy', 'completing', 'wrong_ack'):
        run(sys.argv[1], mode)
