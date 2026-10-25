#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A source sending one byte per data packet must not stall reception.

Each packet becomes one buffered write. Inserting it with a list scan, and flushing
on every packet, made each packet walk all pending writes: the download below took
over a minute with the main thread at full CPU.
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

from AllSearchIntegrationTest import C, connect_daemon, free_ports, retry_port_collisions, stop_daemon
from CancelCompletedIntegrationTest import md4
from DaemonConfig import write_amule_conf
from DownloadDisplayRatesIntegrationTest import hello, local_ipv4
from ED2KLinkIntegrationTest import add_link
from UploadRefillIntegrationTest import packet, send

SIZE = 8 * 184320
DEADLINE = 30
DATA = bytes(i * 7 % 251 for i in range(SIZE))


class Source:
    def __init__(self):
        self.address = local_ipv4()
        self.name = b'tiny-packets.bin'
        self.file_hash = md4(DATA)
        self.identity = socket.socket()
        self.identity.bind((self.address, 0))
        self.stopped = threading.Event()
        self.sent = 0
        self.errors = []

    def link(self):
        return (f'ed2k://|file|{self.name.decode()}|{SIZE}|{self.file_hash.hex()}|'
                f'sources,{self.address}:{self.identity.getsockname()[1]}|/')

    def serve(self, port):
        try:
            with socket.socket() as peer:
                peer.settimeout(10)
                peer.bind((self.address, 0))
                peer.connect((self.address, port))
                send(peer, 0x01, hello(self.name, self.address, self.identity.getsockname()[1]))
                while not self.stopped.is_set():
                    if not select.select([peer], [], [], 0.1)[0]:
                        continue
                    _, opcode, data = packet(peer)
                    if opcode in (0x4c, 0x58, 0x4f):
                        if opcode != 0x4f:
                            send(peer, 0x59, self.file_hash + struct.pack('<H', len(self.name)) + self.name)
                        send(peer, 0x50, self.file_hash + bytes(2))
                    elif opcode == 0x54:
                        send(peer, 0x55)
                    elif opcode == 0x47:
                        offsets = struct.unpack_from('<6I', data, 16)
                        for start, end in zip(offsets[:3], offsets[3:]):
                            # 30 wire bytes per byte of payload, sent in one call per range.
                            peer.sendall(b''.join(
                                struct.pack('<BIB', 0xe3, 26, 0x46) + self.file_hash
                                + struct.pack('<II', offset, offset + 1) + DATA[offset:offset + 1]
                                for offset in range(start, end)))
                            self.sent += max(0, end - start)
        except (EOFError, OSError) as error:
            if not self.stopped.is_set():
                self.errors.append(repr(error))


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-tiny-packets-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        source = Source()
        ports = free_ports(2, peer_index=1)
        write_amule_conf(root, ports[0], Address=source.address, Port=ports[1], UDPEnable=0,
                         FilterLanIPs=0, MaxDownload=0, MaxUpload=0)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        env.pop('LD_PRELOAD', None)
        proc = ec = worker = None
        try:
            with (root / 'console.log').open('w') as log:
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], env=env,
                                        stdout=log, stderr=log)
            ec = connect_daemon(proc, ports[0], root, ports[1])
            assert add_link(ec, source.link()) == C['EC_OP_NOOP']
            worker = threading.Thread(target=source.serve, args=(ports[1],), daemon=True)
            worker.start()
            done = root / 'Incoming' / source.name.decode()
            started = time.monotonic()
            while not done.exists():
                assert time.monotonic() - started < DEADLINE, (
                    'download did not complete', source.sent, source.errors,
                    (root / 'logfile').read_text(errors='replace')[-4000:])
                time.sleep(0.1)
            elapsed = time.monotonic() - started
            time.sleep(0.5)
            assert done.read_bytes() == DATA, 'completed file differs from the source data'
            assert not source.errors, source.errors
            print(f'PASS: {SIZE} one-byte packets completed in {elapsed:.1f} s', flush=True)
        finally:
            source.stopped.set()
            source.identity.close()
            if worker:
                worker.join(timeout=2)
            if ec:
                ec.sock.close()
            if proc:
                stop_daemon(proc)


if __name__ == '__main__':
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve())))
