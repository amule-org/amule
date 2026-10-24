#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Servers known by name get global source requests for a new download, never a paused one.

A fake server answers the TCP login. Two more servers are `localhost:<port>` entries, so their
UDP packets need a name lookup; their UDP ports are listeners that record each request.
"""
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import C, connect_daemon, exact, free_ports, retry_port_collisions, stop_daemon, string
from DaemonConfig import write_amule_conf

OP_GLOBGETSOURCES = 0x9a
OP_GLOBGETSOURCES2 = 0x94


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-global-sources-') as directory:
        root = Path(directory)
        ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToED2K=1,
                         FilterLanIPs=0, AddNewFilesPaused=1)
        listeners = []
        for _ in range(2):
            listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            listener.bind(('127.0.0.1', 0))
            listeners.append(listener)
        login = socket.socket()
        login.bind(('127.0.0.1', 0))
        login.listen(1)
        stop = threading.Event()
        logged_in = threading.Event()
        asked = {listener.getsockname()[1] - 4: [] for listener in listeners}

        def record():
            while not stop.is_set():
                for listener in select.select(listeners, [], [], 0.2)[0]:
                    data = listener.recv(4096)
                    if len(data) > 2 and data[0] == 0xe3 and data[1] in (OP_GLOBGETSOURCES, OP_GLOBGETSOURCES2):
                        step = 16 if data[1] == OP_GLOBGETSOURCES else 20
                        asked[listener.getsockname()[1] - 4] += [data[i:i + 16] for i in range(2, len(data) - 15, step)]

        def serve():
            try:
                peer = login.accept()[0]
                with peer:
                    while not stop.is_set():
                        _, length = struct.unpack('<BI', exact(peer, 5))
                        if exact(peer, length)[0] == 0x01:  # OP_LOGINREQUEST -> OP_IDCHANGE, high ID
                            body = b'\x40' + struct.pack('<II', 0x12345678, 0)
                            peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
                            logged_in.set()
            except (EOFError, OSError):
                pass

        workers = [threading.Thread(target=record, daemon=True), threading.Thread(target=serve, daemon=True)]
        for worker in workers:
            worker.start()
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        with (root / 'stdout.log').open('w') as log:
            proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
            try:
                ec = connect_daemon(proc, ec_port, root, peer_port, udp_port, server_udp=True)

                def add(address):
                    return ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], address)])[0]

                assert add(f'localhost:{login.getsockname()[1]}') == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                assert logged_in.wait(30), 'the daemon did not log in'
                for port in asked:
                    assert add(f'localhost:{port}') == C['EC_OP_NOOP']
                # AddNewFilesPaused leaves the first download of an empty queue running.
                active, paused = os.urandom(16), os.urandom(16)
                for index, file_hash in enumerate((active, paused)):
                    link = f'ed2k://|file|global-{index}.bin|1048576|{file_hash.hex()}|/'
                    assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                deadline = time.monotonic() + 30
                while not all(active in hashes for hashes in asked.values()):
                    assert time.monotonic() < deadline, f'servers known by name were not asked: {asked}'
                    time.sleep(0.2)
                assert not any(paused in hashes for hashes in asked.values()), 'a paused download was asked'
                ec.sock.close()
            except BaseException:
                logfile = root / 'logfile'
                print(logfile.read_text(errors='replace')[-5000:] if logfile.exists() else 'no logfile')
                raise
            finally:
                stop.set()
                workers[0].join()
                for sock in listeners + [login]:
                    sock.close()
                stop_daemon(proc)
        print('both servers known by name were asked about the new download, not the paused one')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
