#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real peers must stop requesting under buffered pressure and resume after disk drain."""
import hashlib
import os
from pathlib import Path
import select
import socket
import struct
import sys
import tempfile
import threading
import time
import zlib

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, start_daemon, stop_daemon
from DaemonConfig import daemon_env, write_amule_conf
from CancelCompletedIntegrationTest import md4
from DownloadDisplayRatesIntegrationTest import hello, local_ipv4
from ED2KLinkIntegrationTest import add_link
from UploadRefillIntegrationTest import packet, send

LIMIT = 16 * 1024 * 1024
BLOCK = 184320


class Source:
    def __init__(self, index, compressed, response_gate):
        self.response_gate = response_gate
        self.address = local_ipv4()
        self.name = f'write-pressure-{index}.bin'.encode()
        self.hashes = b''.join(hashlib.md5(self.name + bytes([part])).digest() for part in range(7))
        self.file_hash = md4(self.hashes)
        self.compressed = compressed
        self.identity = socket.socket()
        self.identity.bind((self.address, 0))
        self.peer = None
        self.stopped = threading.Event()
        self.first_start = None
        self.requested = 0
        self.cancelled = 0
        self.opcodes = []
        self.errors = []

    def link(self):
        return (f'ed2k://|file|{self.name.decode()}|67108864|{self.file_hash.hex()}|'
                f'sources,{self.address}:{self.identity.getsockname()[1]}|/')

    def start(self, port):
        self.worker = threading.Thread(target=self.serve, args=(port,), daemon=True)
        self.worker.start()

    def close(self):
        self.stopped.set()
        if self.peer:
            self.peer.close()
        self.identity.close()
        self.worker.join(timeout=2)

    def serve(self, port):
        try:
            with socket.socket() as peer:
                self.peer = peer
                peer.settimeout(5)
                peer.bind((self.address, 0))
                peer.connect((self.address, port))
                send(peer, 0x01, hello(self.name, self.address, self.identity.getsockname()[1]))
                while not self.stopped.is_set():
                    if not select.select([peer], [], [], 0.1)[0]:
                        continue
                    _, opcode, data = packet(peer)
                    self.opcodes.append(hex(opcode))
                    if opcode in (0x4c, 0x58):
                        send(peer, 0x59, self.file_hash + struct.pack('<H', len(self.name)) + self.name)
                        send(peer, 0x50, self.file_hash + bytes(2))
                    elif opcode == 0x4f:
                        # Seven parts; the source has them all.
                        send(peer, 0x50, self.file_hash + bytes(2))
                    elif opcode == 0x51:
                        send(peer, 0x52, self.file_hash + struct.pack('<H', 7) + self.hashes)
                    elif opcode == 0x54:
                        send(peer, 0x55)
                    elif opcode == 0x56:
                        self.cancelled += 1
                    elif opcode == 0x47:
                        offsets = struct.unpack_from('<6I', data, 16)
                        self.requested += sum(max(0, end - start)
                                              for start, end in zip(offsets[:3], offsets[3:]))
                        for start, end in zip(offsets[:3], offsets[3:]):
                            if end <= start:
                                continue
                            if self.first_start is None:
                                self.first_start = start
                            while not self.response_gate.wait(0.1):
                                if self.stopped.is_set():
                                    return
                            if self.compressed:
                                packed = zlib.compress(b'x' * (end - start))
                                payload = self.file_hash + struct.pack('<II', start, len(packed)) + packed
                                peer.sendall(struct.pack('<BIB', 0xc5, len(payload) + 1, 0x40) + payload)
                            else:
                                # An overlapping duplicate must not allocate its bytes twice.
                                first = min(end, start + 4096)
                                send(peer, 0x46, self.file_hash + struct.pack('<II', start, first)
                                     + b'x' * (first - start))
                                for offset in range(start, end, 16384):
                                    last = min(end, offset + 16384)
                                    send(peer, 0x46, self.file_hash + struct.pack('<II', offset, last)
                                         + b'x' * (last - offset))
        except (EOFError, OSError) as error:
            if not self.stopped.is_set():
                self.errors.append(repr(error))


def wait_for(condition, timeout, detail):
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, detail()
        time.sleep(0.05)


def wait_for_plateau(total, limit, detail):
    # Reaching the near-limit threshold can leave final requests in flight.
    # Require a quiet interval, checking the hard cap on every sample.
    deadline = time.monotonic() + 5
    settled = total()
    stable_since = time.monotonic()
    while time.monotonic() < deadline:
        current = total()
        assert current <= limit, detail()
        if current != settled:
            settled = current
            stable_since = time.monotonic()
        if time.monotonic() - stable_since >= 1.5:
            return settled
        time.sleep(0.05)
    raise AssertionError(('requests did not settle', detail()))


def run(binary, shim, compressed, held_sources=False):
    with tempfile.TemporaryDirectory(prefix='amule-write-pressure-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        gate = root / 'stall-writes'
        gate.touch()
        failure = root / 'fail-writes'
        response_gate = threading.Event()
        if not held_sources:
            response_gate.set()
        sources = [Source(index, compressed, response_gate)
                   for index in range(36 if held_sources else 2)]
        ports = free_ports(2, peer_index=1)
        write_amule_conf(root, ports[0], Address=sources[0].address, Port=ports[1], UDPEnable=0,
                         FilterLanIPs=0, MaxDownload=0, MaxUpload=0,
                         MaxConnections=1000, MaxConnectionsPerFiveSeconds=1000,
                         WriteBufferLimitMiB=LIMIT // (1024 * 1024))
        preload = ':'.join(filter(None, [os.environ.get('AMULE_TEST_PRELOAD_RUNTIME'), shim]))
        env = daemon_env(root, LD_PRELOAD=preload, AMULE_TEST_WRITE_GATE=gate,
                         AMULE_TEST_WRITE_ERROR=failure)
        proc = ec = None
        try:
            proc, ec = start_daemon(binary, root, ports, env=env)
            def rss_bytes():
                for line in Path(f'/proc/{proc.pid}/status').read_text().splitlines():
                    if line.startswith('VmRSS:'):
                        return int(line.split()[1]) * 1024
                raise AssertionError('daemon RSS unavailable')
            initial_rss = rss_bytes()
            for source in sources:
                assert add_link(ec, source.link()) == C['EC_OP_NOOP']
                source.start(ports[1])
            total = lambda: sum(source.requested for source in sources)
            detail = lambda: (total(), [(s.errors, s.opcodes[-20:]) for s in sources],
                              (root / 'logfile').read_text(errors='replace')[-4000:])
            if held_sources:
                # Each source withholds its first response. Requests alone must
                # exceed the old volume cap without consuming buffer capacity.
                wait_for(lambda: total() > LIMIT, 30, detail)
                response_gate.set()
            wait_for(lambda: total() >= LIMIT * 3 // 4, 15, detail)
            settled = wait_for_plateau(total, LIMIT + len(sources) * 24 * BLOCK, detail)
            assert not any(s.cancelled or s.errors for s in sources), detail()
            # Include all outstanding pipelines plus scratch/metadata headroom.
            # Sample through a prolonged stall to catch growth after admission stops.
            rss_bound = initial_rss + LIMIT + len(sources) * 24 * BLOCK + 32 * 1024 * 1024
            peak_rss = rss_bytes()
            for _ in range(20):
                peak_rss = max(peak_rss, rss_bytes())
                assert peak_rss <= rss_bound, ('RSS exceeded payload envelope', peak_rss, rss_bound)
                assert total() == settled, ('requests grew during stall', detail())
                time.sleep(0.1)
            # EC responsiveness proves the writer stall did not block the main thread.
            ec.call(C['EC_OP_GET_DLOAD_QUEUE'])
            # Errors must retain the same dirty payload and prevent new requests.
            failure.touch()
            gate.unlink()
            wait_for(lambda: Path(str(failure) + '.observed').exists(), 3, detail)
            time.sleep(0.5)
            assert total() == settled, ('write errors released capacity', detail())
            assert rss_bytes() <= rss_bound, 'RSS grew beyond the envelope after write failure'
            failure.unlink()
            wait_for(lambda: total() > settled + 6 * BLOCK, 10, detail)
            assert not any(s.errors for s in sources), detail()
            def has_written_data():
                for path in (root / 'Temp').glob('*.part'):
                    with path.open('rb') as part:
                        for source in sources:
                            if source.first_start is not None:
                                part.seek(source.first_start)
                                if part.read(32) == b'x' * 32:
                                    return True
                return False
            assert has_written_data(), 'drain resumed requests without writing payload'
            print(f'PASS compressed={compressed}: {settled} requested bytes plateau under stalled '
                  f'writes, peak RSS {peak_rss}, bound {rss_bound}, no source cancellation, '
                  'automatic resume after drain', flush=True)
        finally:
            response_gate.set()
            gate.unlink(missing_ok=True)
            failure.unlink(missing_ok=True)
            for source in sources:
                if hasattr(source, 'worker'):
                    source.close()
                else:
                    source.identity.close()
            if ec:
                ec.sock.close()
            if proc:
                stop_daemon(proc)


if __name__ == '__main__':
    for compressed in (False, True):
        retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve()),
                                         str(Path(sys.argv[2]).resolve()), compressed))

    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve()),
                                     str(Path(sys.argv[2]).resolve()), False, held_sources=True))
