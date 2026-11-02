#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep the text client connected until a loopback SRP peer finishes shutdown."""
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

from ECWire import accept  # Preflight in the parent before starting fixtures.
from AllSearchIntegrationTest import C, exact
from DaemonConfig import daemon_env

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(10)
            self.request = accept(self.request)
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1:
                    body = zlib.decompress(body)
                op = body[0]
                assert op == C['EC_OP_SHUTDOWN'], 'Unexpected EC operation: ' + str(op)
                self.server.received.set()
                if not self.server.release.wait(10) or not self.server.reply:
                    return
                tags = []
                payload = bytes([C['EC_OP_NOOP']]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
                return
        except (EOFError, OSError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


class ShutdownTest(unittest.TestCase):
    def run_peer(self, reply):
        with tempfile.TemporaryDirectory(prefix='amulecmd-shutdown-') as directory:
            with Server(('127.0.0.1', 0), Handler) as server:
                server.received = threading.Event()
                server.release = threading.Event()
                server.reply = reply
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                process = subprocess.Popen([BINARY, '-h', '127.0.0.1',
                    '-p', str(server.server_address[1]), '-P', 'regression',
                    '-c', 'shutdown'],
                    env=daemon_env(directory), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    if not server.received.wait(10):
                        if process.poll() is None:
                            process.kill()
                        stdout, stderr = process.communicate(timeout=10)
                        self.fail('Peer did not receive shutdown: ' + repr((stdout, stderr)))
                    with self.assertRaises(subprocess.TimeoutExpired,
                                           msg='Client exited before shutdown acknowledgment or peer close'):
                        process.wait(timeout=0.25)
                    server.release.set()
                    stdout, stderr = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 0, (stdout, stderr))
                    self.assertNotIn(b'Connection lost', stderr)
                finally:
                    server.release.set()
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=10)
                    server.shutdown()
                    thread.join(5)

    def test_waits_for_shutdown_acknowledgment(self):
        self.run_peer(True)

    def test_accepts_peer_close_without_acknowledgment(self):
        self.run_peer(False)


if __name__ == '__main__':
    unittest.main()
