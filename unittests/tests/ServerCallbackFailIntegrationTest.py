#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A server callback our server refuses must fail at once when the refusal is known to be its own.

A fake server gives the daemon a HighID and answers each source request with LowID sources, so
the daemon asks it to call them back. OP_CALLBACK_FAIL names no request: with one request out it
fails that one at once, and with two out it fails neither, so both wait for the connect timeout.
"""
import os
from pathlib import Path
import queue
import socket
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions,
                                      running_daemon, string)
from DaemonConfig import write_amule_conf

OP_LOGINREQUEST = 0x01
OP_GETSOURCES = 0x19
OP_CALLBACKREQUEST = 0x1c
OP_GETSOURCES_OBFU = 0x23
OP_CALLBACK_FAIL = 0x36
OP_IDCHANGE = 0x40
OP_FOUNDSOURCES = 0x42
# CONNECTION_TIMEOUT: the socket of an unanswered callback times out after it.
TIMEOUT_S = 40


def packet(opcode, body=b''):
    return struct.pack('<BIB', 0xe3, len(body) + 1, opcode) + body


class FakeServer:
    """Logs the daemon in as a HighID and hands out the LowID sources set for each file."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.sources = {}
        self.callbacks = queue.Queue()
        self.peer = None
        self.logged_in = threading.Event()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        try:
            self.peer = self.sock.accept()[0]
            while True:
                _, length = struct.unpack('<BI', exact(self.peer, 5))
                body = exact(self.peer, length)
                opcode, body = body[0], body[1:]
                if opcode == OP_LOGINREQUEST:
                    self.peer.sendall(packet(OP_IDCHANGE, struct.pack('<II', 0x12345678, 0)))
                    self.logged_in.set()
                elif opcode in (OP_GETSOURCES, OP_GETSOURCES_OBFU):
                    ids = self.sources.get(body[:16], ())
                    records = b''.join(struct.pack('<IH', i, 4662) for i in ids)
                    self.peer.sendall(packet(OP_FOUNDSOURCES, body[:16] + bytes([len(ids)]) + records))
                elif opcode == OP_CALLBACKREQUEST:
                    self.callbacks.put(struct.unpack('<I', body[:4])[0])
        except (EOFError, OSError):
            pass

    def refuse(self):
        self.peer.sendall(packet(OP_CALLBACK_FAIL))

    def wait_callbacks(self, ids, timeout=60):
        pending, deadline = set(ids), time.monotonic() + timeout
        while pending:
            pending.discard(self.callbacks.get(timeout=max(0.1, deadline - time.monotonic())))


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-server-callback-fail-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToED2K=1,
                         FilterLanIPs=0, VerboseDebug=1, VerboseDebugLogfile=1,
                         sections={'Debug': {'Cat_ED2k Client': 1, 'Cat_Servers': 1}})
        server = FakeServer()
        log = root / 'logfile'

        def deleted(reason):
            return log.read_text(errors='replace').count(f'Reason was {reason}')

        def wait_log(what, check, timeout):
            deadline = time.monotonic() + timeout
            while not check():
                assert time.monotonic() < deadline, f'timed out waiting for {what}'
                time.sleep(0.2)

        with running_daemon(binary, root, ports, console='stdout.log') as (proc, ec):
            try:
                assert ec.call(C['EC_OP_SERVER_ADD'], [
                    string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{server.port}')])[0] == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                assert server.logged_in.wait(30), 'the daemon did not log in'

                def download(name, ids):
                    file_hash = os.urandom(16)
                    server.sources[file_hash] = ids
                    link = f'ed2k://|file|{name}.bin|1048576|{file_hash.hex()}|/'
                    assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                    server.wait_callbacks(ids)

                # One request out: the refusal is its own, and the source goes at once.
                download('lone', (0x10101,))
                server.refuse()
                wait_log('the refused callback to fail its source', lambda: deleted('Server callback refused') == 1, 10)

                # Two out: the refusal names neither, so both wait for the connect timeout.
                sent = time.monotonic()
                download('pair', (0x20202, 0x30303))
                server.refuse()
                wait_log('the refusal to be left unmatched',
                         lambda: 'OP_CALLBACK_FAIL with 2 callback requests out' in log.read_text(errors='replace'), 10)
                assert deleted('Timeout') == 0, 'a callback timed out early'
                wait_log('both callbacks to time out', lambda: deleted('Timeout') == 2, TIMEOUT_S + 30)
                assert time.monotonic() - sent >= TIMEOUT_S - 1, 'the callbacks timed out before their window'
                assert deleted('Server callback refused') == 1, 'the unmatched refusal failed a request'
                assert proc.poll() is None, 'the daemon exited'
            except BaseException:
                print(log.read_text(errors='replace')[-8000:], file=sys.stderr)
                raise
    print('a lone refusal fails its request at once; an unmatched one leaves both to their timeout')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
