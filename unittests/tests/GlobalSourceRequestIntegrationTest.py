#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Servers known by name get global source requests for a new download, never a paused one.

A fake server answers the TCP login. Three more servers are `localhost:<port>` entries, so their
UDP packets need a name lookup; their UDP ports are listeners that record each request. Two answer
the status ping, and get no request before that answer; the third never answers, and is asked
anyway once the wait for its first status reply is over. After a restart the next pings are hours
away, and every server is asked without one.
"""
import os
from pathlib import Path
import select
import socket
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions,
                                      running_daemon, string, tag)
from DaemonConfig import write_amule_conf

OP_GLOBGETSOURCES = 0x9a
OP_GLOBGETSOURCES2 = 0x94
OP_GLOBSERVSTATREQ = 0x96
OP_GLOBSERVSTATRES = 0x97
FIRST_STATUS_WAIT_S = 30  # kFirstServerStatusWaitS


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-global-sources-') as directory:
        root = Path(directory)
        ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        # No crypt layer: the status ping is then the plain one these listeners can answer.
        write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToED2K=1,
                         FilterLanIPs=0, AddNewFilesPaused=1,
                         sections={'Obfuscation': {'IsClientCryptLayerSupported': 0}})
        listeners = []
        for _ in range(3):
            listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            listener.bind(('127.0.0.1', 0))
            listeners.append(listener)
        login = socket.socket()
        login.bind(('127.0.0.1', 0))
        login.listen(1)
        stop = threading.Event()
        logged_in = threading.Event()
        ports = [listener.getsockname()[1] - 4 for listener in listeners]
        silent = ports[2]
        # One session's records.
        asked = {port: [] for port in ports}
        first_asked, pinged, answered = {}, {}, {}

        def new_session():
            logged_in.clear()
            for records in (first_asked, pinged, answered):
                records.clear()
            for port in ports:
                asked[port] = []

        def record():
            while not stop.is_set():
                for listener in select.select(listeners, [], [], 0.2)[0]:
                    data, sender = listener.recvfrom(4096)
                    port = listener.getsockname()[1] - 4
                    if len(data) >= 6 and data[0] == 0xe3 and data[1] == OP_GLOBSERVSTATREQ:
                        pinged.setdefault(port, time.monotonic())
                        if port != silent:
                            answered.setdefault(port, time.monotonic())
                            # <challenge 4><users 4><files 4>, the shortest answer.
                            listener.sendto(bytes([0xe3, OP_GLOBSERVSTATRES]) + data[2:6] + bytes(8), sender)
                    elif len(data) > 2 and data[0] == 0xe3 and data[1] in (OP_GLOBGETSOURCES, OP_GLOBGETSOURCES2):
                        step = 16 if data[1] == OP_GLOBGETSOURCES else 20
                        first_asked.setdefault(port, time.monotonic())
                        asked[port] += [data[i:i + 16] for i in range(2, len(data) - 15, step)]

        def serve():
            while not stop.is_set():
                try:
                    peer = login.accept()[0]
                except OSError:
                    return
                with peer:
                    try:
                        while not stop.is_set():
                            _, length = struct.unpack('<BI', exact(peer, 5))
                            if exact(peer, length)[0] == 0x01:  # OP_LOGINREQUEST -> OP_IDCHANGE, high ID
                                body = b'\x40' + struct.pack('<II', 0x12345678, 0)
                                peer.sendall(struct.pack('<BI', 0xe3, len(body)) + body)
                                logged_in.set()
                    except (EOFError, OSError):
                        pass

        def wait_until_asked(seconds):
            deadline = time.monotonic() + seconds
            while not all(active in hashes for hashes in asked.values()):
                assert time.monotonic() < deadline, f'servers known by name were not asked: {asked}'
                time.sleep(0.2)
            assert not any(paused in hashes for hashes in asked.values()), 'a paused download was asked'

        workers = [threading.Thread(target=record, daemon=True), threading.Thread(target=serve, daemon=True)]
        for worker in workers:
            worker.start()
        # Both files start paused; explicitly resume the one that should request sources.
        active, paused = os.urandom(16), os.urandom(16)
        try:
            with running_daemon(binary, root, (ec_port, peer_port, udp_port),
                                console='stdout.log', server_udp=True) as (proc, ec):
                def add(address):
                    return ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'], address)])[0]

                assert add(f'localhost:{login.getsockname()[1]}') == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                assert logged_in.wait(30), 'the daemon did not log in'
                for port in ports:
                    assert add(f'localhost:{port}') == C['EC_OP_NOOP']
                for index, file_hash in enumerate((active, paused)):
                    link = f'ed2k://|file|global-{index}.bin|1048576|{file_hash.hex()}|/'
                    assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_PARTFILE_RESUME'], [
                    tag(C['EC_TAG_PARTFILE'], active, 9)])[0] == C['EC_OP_NOOP']
                # One status ping every 5 s, then the silent server's wait.
                wait_until_asked(30 + FIRST_STATUS_WAIT_S + 20)
                for port in ports:
                    if port == silent:
                        assert first_asked[port] >= pinged[port] + FIRST_STATUS_WAIT_S - 2, \
                            'a server was asked before its first status reply or the end of the wait'
                    else:
                        assert first_asked[port] >= answered[port], 'a server was asked before its status reply'

            # server.met now holds no UDP flags, as the answers had none, and a last ping that puts
            # the next one hours away.
            new_session()
            with running_daemon(binary, root, (ec_port, peer_port, udp_port),
                                console='stdout.log', server_udp=True) as (proc, ec):
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                assert logged_in.wait(30), 'the daemon did not log in again'
                wait_until_asked(30)
                assert not pinged, f'a server was pinged again after the restart: {pinged}'
        finally:
            stop.set()
            workers[0].join()
            for sock in listeners + [login]:
                sock.close()
        print('the servers known by name were asked about the new download after their first status '
              'reply or the wait, and after a restart without one, never about the paused one')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
