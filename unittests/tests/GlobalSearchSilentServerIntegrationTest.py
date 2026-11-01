#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A global search asks the other servers when the connected one never answers.

Lugdunum servers send no OP_SEARCHRESULT when they have no match. The fake connected server
holds its answer past the daemon's timeout, then sends it while the UDP sweep runs: the late
answer must be filed without restarting the sweep, so each server is asked exactly once.
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

from AllSearchIntegrationTest import (C, connect_daemon, exact, free_ports, retry_port_collisions,
                                      search_record, stop_daemon, string)
from DaemonConfig import write_amule_conf

OP_SEARCHREQUEST = 0x16
OP_SEARCHRESULT = 0x33
OP_GLOBSEARCHREQS = (0x90, 0x92, 0x98)
QUERY = b'silentserver'
SERVERS = 6


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-global-search-') as directory:
        root = Path(directory)
        ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToED2K=1,
                         FilterLanIPs=0)
        listeners = []
        for _ in range(SERVERS):
            listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            listener.bind(('127.0.0.1', 0))
            listeners.append(listener)
        login = socket.socket()
        login.bind(('127.0.0.1', 0))
        login.listen(1)
        stop = threading.Event()
        logged_in = threading.Event()
        searched = threading.Event()
        answer = threading.Event()
        asked = {listener.getsockname()[1] - 4: 0 for listener in listeners}

        def record():
            while not stop.is_set():
                for listener in select.select(listeners, [], [], 0.2)[0]:
                    data = listener.recv(4096)
                    if len(data) > 2 and data[0] == 0xe3 and data[1] in OP_GLOBSEARCHREQS and QUERY in data:
                        asked[listener.getsockname()[1] - 4] += 1

        def serve():
            try:
                peer = login.accept()[0]
                with peer:
                    while not stop.is_set():
                        _, length = struct.unpack('<BI', exact(peer, 5))
                        opcode = exact(peer, length)[0]
                        if opcode == 0x01:  # OP_LOGINREQUEST -> OP_IDCHANGE, high ID
                            body = b'\x40' + struct.pack('<II', 0x12345678, 0)
                            peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
                            logged_in.set()
                        elif opcode == OP_SEARCHREQUEST:
                            searched.set()
                            answer.wait()
                            if stop.is_set():
                                return
                            body = bytes([OP_SEARCHRESULT]) + struct.pack('<I', 1) + search_record('late.bin')
                            peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
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
                sid = ec.start(QUERY.decode(), kind=C['EC_SEARCH_GLOBAL'])
                assert searched.wait(5), 'the connected server got no search request'
                deadline = time.monotonic() + 30
                while not any(asked.values()):
                    assert time.monotonic() < deadline, 'no other server was asked after the timeout'
                    time.sleep(0.1)
                answer.set()
                deadline = time.monotonic() + 30
                while True:
                    state = ec.progress(sid)
                    if state[C['EC_TAG_SEARCH_LIFECYCLE_STATE']][0] == 2:
                        break
                    assert time.monotonic() < deadline, f'the sweep did not finish: {state}'
                    time.sleep(0.2)
                time.sleep(1.5)
                assert all(count == 1 for count in asked.values()), f'each server must be asked once: {asked}'
                assert state[C['EC_TAG_SEARCH_RESULT_COUNT']][0] == 1, state
                ec.sock.close()
            except BaseException:
                logfile = root / 'logfile'
                print(logfile.read_text(errors='replace')[-5000:] if logfile.exists() else 'no logfile')
                raise
            finally:
                stop.set()
                answer.set()
                workers[0].join()
                for sock in listeners + [login]:
                    sock.close()
                stop_daemon(proc)
        print('the silent server timed out, every other server was asked once, the late answer was kept')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
