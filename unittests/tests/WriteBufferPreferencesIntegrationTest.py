#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A new API must not claim to change a write budget on an older daemon."""
import os
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import zlib

from ECWire import accept
from AllSearchIntegrationTest import C, exact, free_ports, integer, tag
from AmuleApiEcPasswordIntegrationTest import stop
from CompletedFileDeleteIntegrationTest import eventually, http

LIMIT_TAG = C['EC_TAG_CORETW_WRITE_BUFFER_LIMIT_MIB']


class CoreHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request = accept(self.request)
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1:
                    body = zlib.decompress(body)
                op = body[0]
                reply, tags = C['EC_OP_NOOP'], []
                if op == C['EC_OP_GET_PREFERENCES']:
                    fields = [integer(C['EC_TAG_CORETW_MAX_CONN_PER_FIVE'], self.server.connections)]
                    if self.server.supported:
                        fields.append(integer(LIMIT_TAG, self.server.limit))
                    reply = C['EC_OP_SET_PREFERENCES']
                    tags = [tag(C['EC_TAG_PREFS_CORETWEAKS'], children=fields)]
                elif op == C['EC_OP_SET_PREFERENCES']:
                    self.server.writes.append(body)
                payload = bytes([reply]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        except (EOFError, OSError):
            pass


class Core(socketserver.ThreadingTCPServer):
    daemon_threads = True


def run(api_binary, supported):
    with tempfile.TemporaryDirectory(prefix='amule-write-budget-prefs-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, HOME=temporary, XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        env.pop('LD_PRELOAD', None)
        with Core(('127.0.0.1', 0), CoreHandler) as core, (root / 'api.log').open('w') as log:
            core.supported, core.limit, core.connections, core.writes = supported, 128, 50, []
            threading.Thread(target=core.serve_forever, daemon=True).start()
            command = [api_binary, '--config-dir=' + temporary]
            subprocess.run(command + ['--set-admin-pass-stdin'], input=b'regression\n',
                           env=env, check=True, stdout=log, stderr=log, timeout=10)
            (root / 'amuleapi.conf').write_text('[EC]\nPassword=regression\n')
            (root / 'amuleapi.conf').chmod(0o600)
            port = free_ports(1)[0]
            base = f'http://127.0.0.1:{port}/api/v1'
            proc = subprocess.Popen(command + ['--host=127.0.0.1',
                '--port=' + str(core.server_address[1]), '--http-port=' + str(port)],
                env=env, stdout=log, stderr=log)
            try:
                def ready():
                    assert proc.poll() is None, (root / 'api.log').read_text()
                    try:
                        return http(base, '/health')[1].get('ec_connected')
                    except OSError:
                        return False
                eventually(ready)
                token = http(base, '/auth/login?include_token=true', method='POST',
                             body={'password': 'regression'})[1]['token']
                # A client must never report success for an ignored write, even if the
                # caller tries to set the read-only capability flag itself.
                status, result = http(base, '/preferences', token, 'PATCH', {'advanced': {
                    'write_buffer_limit_supported': True, 'write_buffer_limit_mebibytes': 128}})
                assert status == (200 if supported else 409), (status, result)
                if supported:
                    assert result['advanced']['write_buffer_limit_mebibytes'] == 128
                    assert len(core.writes) == 1, 'supported write did not reach the daemon'
                else:
                    assert result['error']['code'] == 'option_not_supported', result
                    assert not core.writes, 'unsupported write reached the legacy daemon'
                prefs = http(base, '/preferences', token)[1]['advanced']
                assert prefs['write_buffer_limit_supported'] is supported, prefs
                assert prefs['write_buffer_limit_mebibytes'] == (128 if supported else 0), prefs
                previous_writes = len(core.writes)
                status, result = http(base, '/preferences', token, 'PATCH', {'advanced': {
                    'max_new_connections_per_5_seconds': 51}})
                assert status == 200, result
                assert len(core.writes) == previous_writes + 1, 'unrelated save did not reach the daemon'
                print('PASS: write budget supported =', supported)
            except BaseException:
                print((root / 'api.log').read_text(errors='replace'), file=sys.stderr)
                raise
            finally:
                stop(proc)
                core.shutdown()


if __name__ == '__main__':
    for supported in (False, True):
        run(str(Path(sys.argv[1]).resolve()), supported)
