#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Startup updates still merge all ordered addresses.dat sources without rewriting it."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import free_ports, retry_port_collisions, stop_daemon
from DaemonConfig import write_amule_conf
from KnownHistoryIntegrationTest import start


def server_met(last):
    return b'\xe0' + struct.pack('<I', 1) + bytes([203, 0, 113, last]) + struct.pack('<HI', 4661, 0)


def run(binary, root):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            body = server_met(7 if self.path == '/first' else 10)
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    proc = ec = None
    try:
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        (root / 'server.met').write_bytes(server_met(9))
        base = f'http://127.0.0.1:{server.server_port}'
        sources = (f'# user-maintained list\n\n{base}/first\n{base}/first\n'
                   f'ftp://unsupported/list\n{base}/last\n')
        source_file = root / 'addresses.dat'
        source_file.write_text(sources)
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2],
                         ConnectToED2K=1, Serverlist=1)
        proc, ec = start(binary, root, ports)
        deadline = time.monotonic() + 20
        while True:
            data = (root / 'server.met').read_bytes() if (root / 'server.met').exists() else b''
            if all(bytes([203, 0, 113, last]) in data for last in (7, 9, 10)):
                break
            if proc.poll() is not None or time.monotonic() > deadline:
                raise AssertionError((requests, (root / 'logfile').read_text(errors='replace')))
            time.sleep(0.05)
        time.sleep(0.3)
        assert requests == ['/first', '/last'], requests
        assert int.from_bytes(data[1:5], 'little') == 3, data
        assert source_file.read_text() == sources
    finally:
        if ec:
            ec.sock.close()
        if proc:
            stop_daemon(proc)
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


if __name__ == '__main__':
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-server-sources-') as temp:
            run(binary, Path(temp))

    retry_port_collisions(attempt)
    print('startup server-list sources passed')
