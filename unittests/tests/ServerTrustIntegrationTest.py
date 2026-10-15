#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A connected eD2k server must not direct the client past the user's settings.

It could add server-list entries the user did not ask for, call the client back to loopback,
and point a hostname entry at a loopback or LAN address.
"""
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions, stop_daemon,
                                      string)
from DaemonConfig import write_amule_conf
from KnownHistoryIntegrationTest import start

INJECTED = bytes([203, 0, 113, 7])  # not a reserved range, so AddServer() accepts it
MARKER = 'server-trust-marker'


def packet(opcode, body):
    return struct.pack('<BIB', 0xe3, len(body) + 1, opcode) + body


class Listener:
    """Counts the TCP connections made to it."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.accepted = []
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        try:
            while True:
                self.accepted.append(self.sock.accept()[0])
        except OSError:
            pass


def fake_server(callback_port):
    """After login: an unrequested server list, a loopback callback, then a marker message."""
    listener = Listener()

    def serve():
        while not listener.accepted:
            time.sleep(0.05)
        peer = listener.accepted[0]
        try:
            while True:
                _, length = struct.unpack('<BI', exact(peer, 5))
                if exact(peer, length)[0] != 0x01:  # OP_LOGINREQUEST
                    continue
                message = f'WARNING: {MARKER}'.encode()
                peer.sendall(packet(0x40, struct.pack('<II', 0x12345678, 0))  # OP_IDCHANGE
                             + packet(0x32, b'\x01' + INJECTED + struct.pack('<H', 4661))  # OP_SERVERLIST
                             + packet(0x35, bytes([127, 0, 0, 1]) + struct.pack('<H', callback_port))  # OP_CALLBACKREQUESTED
                             + packet(0x38, struct.pack('<H', len(message)) + message))  # OP_SERVERMESSAGE
        except (EOFError, OSError):
            pass

    threading.Thread(target=serve, daemon=True).start()
    return listener


def wait_for(proc, what, check, timeout=60):
    deadline = time.monotonic() + timeout
    while not check():
        if proc.poll() is not None:
            raise AssertionError(f'daemon exited while waiting for {what}')
        if time.monotonic() > deadline:
            raise AssertionError(f'timed out waiting for {what}')
        time.sleep(0.2)


def configure(root, ports, filter_lan):
    ec_port, peer_port, udp_port = ports
    write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToED2K=1,
                     FilterLanIPs=int(filter_lan))


def add_and_connect(ec, port):
    assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{port}')])[0] == C['EC_OP_NOOP']
    assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']


def log(root):
    return (root / 'logfile').read_text(errors='replace')


def run(binary, root):
    for d in ('Incoming', 'Temp'):
        (root / d).mkdir()
    ports = free_ports(3, udp_indices=(2,), peer_index=1)

    # With LAN filtering on, a hostname that resolves to loopback is not dialled.
    configure(root, ports, filter_lan=True)
    server = Listener()
    proc, ec = start(binary, root, ports)
    try:
        add_and_connect(ec, server.port)
        wait_for(proc, 'the loopback server to be refused',
                 lambda: 'Server IP 127.0.0.1 (localhost) is filtered' in log(root))
        assert not server.accepted, 'the client connected to a loopback server with LAN filtering on'
    finally:
        ec.sock.close()
        stop_daemon(proc)

    # With it off, the local server is reached, but it still cannot add servers the user did
    # not ask for or have the client call back to loopback.
    (root / 'server.met').unlink(missing_ok=True)
    configure(root, ports, filter_lan=False)
    callback = Listener()
    server = fake_server(callback.port)
    proc, ec = start(binary, root, ports)
    try:
        add_and_connect(ec, server.port)
        wait_for(proc, 'the marker server message', lambda: MARKER in log(root))
        time.sleep(2)  # a callback connect, if one was started, lands well within this
        assert not callback.accepted, 'the client called back to a loopback address'
        assert 'Received 1 new server' not in log(root), 'an unrequested server list was applied'
    finally:
        ec.sock.close()
        stop_daemon(proc)
    assert INJECTED not in (root / 'server.met').read_bytes(), 'the injected server was saved'


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-server-trust-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    print('Server trust checks passed')


if __name__ == '__main__':
    main()
