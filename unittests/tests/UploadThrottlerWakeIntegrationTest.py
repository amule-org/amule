#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A fast upload slot does not stall while the throttler sleeps.

One loopback peer with a small receive buffer reads at a steady 16 MB/s, with no upload limit,
and always keeps 24 blocks requested. Its socket is often full, so the throttler often sends nothing
and backs its sleep off towards 1 s, while the small buffer drains in a few milliseconds. Unless the
socket wakes the throttler once it can take data again, the peer then waits for the rest of that
sleep: without the wake, a 6 s window had 5-6 pauses of 450-850 ms between data packets; with it,
none longer than 50 ms.
Example: python3 UploadThrottlerWakeIntegrationTest.py build/src/amuled
"""
import argparse
import hashlib
import os
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time

from AllSearchIntegrationTest import free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import wait_shared
from UploadRefillIntegrationTest import BLOCK, packet, send

OP_HELLO = 0x01
OP_HELLOANSWER = 0x4c
OP_SETREQFILEID = 0x4f
OP_STARTUPLOADREQ = 0x54
OP_ACCEPTUPLOADREQ = 0x55
OP_REQUESTPARTS = 0x47
OP_SENDINGPART = 0x46
PACE = 16e6          # bytes per second the peer reads
RECEIVE_BUFFER = 16384
PAUSE_MS = 250       # longer than any pause with the wake, shorter than those without it
ALLOWED_PAUSES = 1   # one for a scheduling hiccup on a busy machine


def run(binary, warmup, seconds):
    content = os.urandom(16 * 1024 * 1024)
    blocks = len(content) // BLOCK
    with tempfile.TemporaryDirectory(prefix='amule-throttler-wake-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        (root / 'Incoming' / 'wake.bin').write_bytes(content)
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0, MaxUpload=0,
                         SlotAllocation=10)
        stop = threading.Event()
        started = threading.Event()
        arrivals = []
        errors = []
        with running_daemon(binary, root, (ec_port, peer_port)) as (proc, ec):
            file_hash = wait_shared(proc, ec, ['wake.bin'], 60)['wake.bin']
            sock = socket.socket()

            def peer():
                try:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RECEIVE_BUFFER)
                    sock.bind(('127.0.0.2', 0))
                    sock.settimeout(40)
                    sock.connect(('127.0.0.1', peer_port))
                    name = b'throttler-wake-peer'
                    send(sock, OP_HELLO, b'\x10' + hashlib.md5(name).digest() + socket.inet_aton('127.0.0.2')
                         + struct.pack('<HI', sock.getsockname()[1], 1)
                         + b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name + bytes(6))
                    while packet(sock)[1] != OP_HELLOANSWER:
                        pass
                    send(sock, OP_SETREQFILEID, file_hash)
                    send(sock, OP_STARTUPLOADREQ, file_hash)
                    while packet(sock)[1] != OP_ACCEPTUPLOADREQ:
                        pass
                    next_block, pending, received = 0, 0, 0

                    def request():
                        nonlocal next_block, pending
                        while pending <= 21 * BLOCK:
                            starts = [(next_block + i) % blocks * BLOCK for i in range(3)]
                            next_block = (next_block + 3) % blocks
                            send(sock, OP_REQUESTPARTS,
                                 file_hash + struct.pack('<6I', *starts, *(s + BLOCK for s in starts)))
                            pending += 3 * BLOCK

                    request()
                    started.set()
                    begin = time.monotonic()
                    while not stop.is_set():
                        ahead = received / PACE - (time.monotonic() - begin)
                        if ahead > 0:
                            time.sleep(ahead)
                        proto, opcode, data = packet(sock)
                        if (proto, opcode) != (0xe3, OP_SENDINGPART):
                            continue
                        start, end = struct.unpack_from('<II', data, 16)
                        assert data[24:] == content[start:end], 'upload payload differs'
                        arrivals.append(time.monotonic())
                        received += end - start
                        pending -= end - start
                        request()
                except BaseException as error:
                    if not stop.is_set():
                        errors.append(repr(error))

            thread = threading.Thread(target=peer, daemon=True)
            thread.start()
            try:
                assert started.wait(40), ('the upload slot did not start', errors)
                time.sleep(warmup)
                begin = time.monotonic()
                time.sleep(seconds)
                end = time.monotonic()
                assert not errors, errors
                window = [t for t in arrivals if begin <= t <= end]
                assert len(window) > 1, 'no upload data arrived'
                pauses = [round((b - a) * 1000) for a, b in zip(window, window[1:]) if (b - a) * 1000 > PAUSE_MS]
                assert len(pauses) <= ALLOWED_PAUSES, (
                    f'upload stalled while the throttler slept: pauses of {pauses} ms in {seconds:g} s '
                    f'({len(window)} packets)')
                print(f'OK: {len(window)} packets in {seconds:g} s, pauses over {PAUSE_MS} ms: {pauses}')
            finally:
                stop.set()
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sock.close()
                thread.join(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--warmup', type=float, default=3)
    parser.add_argument('--seconds', type=float, default=6)
    args = parser.parse_args()
    retry_port_collisions(run, str(Path(args.binary).resolve()), args.warmup, args.seconds)
