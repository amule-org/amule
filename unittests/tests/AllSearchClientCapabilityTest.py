#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify API and command-line guards against a loopback EC peer without All support."""
import socketserver, struct, subprocess, sys, tempfile, threading, zlib
from pathlib import Path
from ECWire import accept  # Preflight in the parent before starting fixtures.
from AllSearchIntegrationTest import (C, configure_amuleapi, exact, free_ports, http, integer,
                                      retry_port_collisions, start_amuleapi, stop_daemon, string,
                                      tag, wait_amuleapi)
from DaemonConfig import daemon_env
requests = []
class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request = accept(self.request)
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1: body = zlib.decompress(body)
                op = body[0]
                requests.append(op)
                if op == C['EC_OP_SEARCH_LIST']:
                    reply = C['EC_OP_SEARCH_LIST']
                    tags = [tag(C['EC_TAG_SEARCH_ID'], struct.pack('!I', 101), 4,
                                [string(C['EC_TAG_SEARCH_NAME'], 'legacy form'),
                                 integer(C['EC_TAG_SEARCH_MIN_SIZE'], 0),
                                 integer(C['EC_TAG_SEARCH_MAX_SIZE'], 0),
                                 integer(C['EC_TAG_SEARCH_AVAILABILITY'], 0)])]
                else:
                    reply, tags = C['EC_OP_NOOP'], []
                payload = bytes([reply]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        except (EOFError, OSError): pass
class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


def run(api, cmd):
    with tempfile.TemporaryDirectory(prefix='amule-missing-capability-') as tmp:
        cfg = Path(tmp)
        env = daemon_env(tmp)
        with Server(('127.0.0.1', 0), Handler) as legacy:
            thread=threading.Thread(target=legacy.serve_forever, daemon=True); thread.start()
            port=legacy.server_address[1]; hp=free_ports(1)[0]; url=f'http://127.0.0.1:{hp}/api/v1'
            with (cfg/'api.log').open('w') as log:
                configure_amuleapi(api, cfg, env, log)
                proc=start_amuleapi(api, cfg, port, hp, env, log)
                try:
                    token=wait_amuleapi(proc, url, log.name)
                    status, result=http(url, '/search', token, 'POST', {'query':'legacyguard','type':'all'})
                    assert status==503 and result['error']['code']=='ec_unsupported', (status, result)
                    print('PASS: API returns 503 ec_unsupported without All capability')
                    for filters in ({'min_bitrate_kilobits_per_second': 128}, {'min_duration_seconds': 3723},
                                    {'codec': 'FLAC'}, {'title': 'Title'}, {'album': 'Album'},
                                    {'artist': 'Artist'}):
                        status, result = http(url, '/search', token, 'POST', {'query': 'legacyguard', 'type': 'kad', **filters})
                        assert status == 503 and result['error']['code'] == 'ec_unsupported', (status, result)
                    print('PASS: metadata filters rejected without daemon capability')
                    status, result = http(url, '/search', token)
                    assert status == 200, (status, result)
                    row = next(row for row in result['searches'] if row['search_id'] == 101)
                    for key in ('min_bitrate_kilobits_per_second', 'min_duration_seconds',
                                'codec', 'title', 'album', 'artist'):
                        assert key in row and row[key] is None, (key, row)
                    print('PASS: legacy daemon rows retain all six media keys as null')
                    status, result=http(url, '/status', token)
                    assert status==200 and result['search_all_supported'] is False, (status,result)
                    print('PASS: REST reports absent All capability as false')
                    out=subprocess.run([cmd, '-h','127.0.0.1','-p',str(port),'-P','regression','-c','search all legacyguard'], env=env, capture_output=True, text=True, timeout=15)
                    assert 'does not support All searches' in out.stdout+out.stderr, out
                    assert C['EC_OP_SEARCH_START'] not in requests, requests
                    print('PASS: amulecmd reports unsupported All without sending SEARCH_START')
                except BaseException:
                    log.flush();print((cfg/'api.log').read_text(),file=sys.stderr);raise
                finally:
                    stop_daemon(proc, check=sys.exc_info()[0] is None);legacy.shutdown()


if __name__ == "__main__":
    retry_port_collisions(run, *sys.argv[1:])
