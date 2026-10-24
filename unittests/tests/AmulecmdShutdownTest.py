#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep the text client connected until a loopback peer finishes shutdown."""
import os
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

from AllSearchIntegrationTest import C, exact, integer, string

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(10)
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1:
                    body = zlib.decompress(body)
                op = body[0]
                if op == C['EC_OP_AUTH_REQ']:
                    reply, tags = C['EC_OP_AUTH_SALT'], [integer(C['EC_TAG_PASSWD_SALT'], 123)]
                elif op == C['EC_OP_AUTH_PASSWD']:
                    reply, tags = C['EC_OP_AUTH_OK'], [string(C['EC_TAG_SERVER_VERSION'], 'shutdown-fixture')]
                elif op == C['EC_OP_SHUTDOWN']:
                    self.server.received.set()
                    if not self.server.release.wait(10) or not self.server.reply:
                        return
                    reply, tags = C['EC_OP_NOOP'], []
                else:
                    raise AssertionError('Unexpected EC operation: ' + str(op))
                payload = bytes([reply]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
                if op == C['EC_OP_SHUTDOWN']:
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
                env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=directory + '/xdg', LC_ALL='C.UTF-8')
                process = subprocess.Popen([BINARY, '-h', '127.0.0.1',
                    '-p', str(server.server_address[1]), '-P', 'regression',
                    '--disable-ec-encryption', '-c', 'shutdown'],
                    env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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
