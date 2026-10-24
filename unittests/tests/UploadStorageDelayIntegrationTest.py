#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gate actual upload reads and verify prompt delivery and fair client turns.

Linux only. The preload fixture blocks each read of one shared file until the
parent releases it. Assertions run while the following read is held, so success
cannot depend on a fast disk or an arbitrary sleep. All peers are loopback-only.
"""
import argparse
import hashlib
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time

from AllSearchIntegrationTest import connect_daemon, exact, free_ports, retry_port_collisions, stop_daemon
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared
from UploadRefillIntegrationTest import BLOCK, packet, send


class Peer:
    def __init__(self, port, index, file_hash, compress=False):
        self.file_hash = file_hash
        self.sock = socket.socket()
        try:
            address = f'127.0.0.{index + 2}'
            self.sock.bind((address, 0))
            self.sock.settimeout(5)
            self.sock.connect(('127.0.0.1', port))
            name = f'storage-delay-{index}'.encode()
            tag = b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name
            if compress:
                tag += (b'\x03\x01\x00\xfa' + struct.pack('<I', 1 << 20)
                        + b'\x03\x01\x00\xfb' + struct.pack('<I', 50 << 10))
            hello = (b'\x10' + hashlib.md5(name).digest() + socket.inet_aton(address)
                     + struct.pack('<HI', self.sock.getsockname()[1], 3 if compress else 1) + tag + bytes(6))
            send(self.sock, 0x01, hello)
            self.until(0x4c)
            send(self.sock, 0x4f, file_hash)
            send(self.sock, 0x54, file_hash)
            self.until(0x55)
        except BaseException:
            self.sock.close()
            raise

    def until(self, wanted):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            proto, opcode, data = packet(self.sock)
            if (proto, opcode) == (0xe3, wanted):
                return data
        raise AssertionError(f'peer did not receive opcode {wanted:#x}')

    def request(self, ranges):
        for index in range(0, len(ranges), 3):
            group = ranges[index:index + 3]
            starts = [start for start, _ in group] + [0] * (3 - len(group))
            ends = [end for _, end in group] + [0] * (3 - len(group))
            send(self.sock, 0x47, self.file_hash + struct.pack('<6I', *starts, *ends))

    def barrier(self):
        # TCP ordering: the answer proves the core processed earlier block requests
        # before the disk gate is released. It requests the same file, without a switch.
        send(self.sock, 0x58, self.file_hash)
        assert self.until(0x59)[:16] == self.file_hash

    def receive(self, span, content):
        position, end = span
        try:
            while position < end:
                data = self.until(0x46)
                assert data[:16] == self.file_hash, 'wrong upload file'
                start, stop = struct.unpack_from('<II', data, 16)
                assert start == position and start < stop <= end, 'packet gap or duplicate'
                assert data[24:] == content[start:stop], 'upload data differs'
                position = stop
        except TimeoutError as error:
            raise AssertionError(f'block {span} was not delivered while the next read was held') from error

    def close(self):
        self.sock.close()


def wait_read(notify, offset):
    ready, _, _ = select.select([notify], [], [], 10)
    assert ready, f'disk worker did not reach controlled read at {offset}'
    event = os.read(notify, 8)
    assert len(event) == 8, 'read gate closed unexpectedly'
    actual, = struct.unpack('=Q', event)
    assert actual == offset, f'expected read at {offset}, got {actual}'


def run(binary, fixture, case):
    sizes = {
        'small': [4096] * 12,
        'full': [BLOCK] * 12,
        'large': [3 * BLOCK] * 12,
        'mixed': [4096, BLOCK, 3 * BLOCK] * 4,
    }[case]
    ranges = []
    offset = 0
    for size in sizes:
        ranges.append((offset, offset + size))
        offset += size
    with tempfile.TemporaryDirectory(prefix='amule-upload-storage-') as directory:
        root = Path(directory)
        incoming = root / 'Incoming'
        incoming.mkdir()
        (root / 'Temp').mkdir()
        content = os.urandom(offset)
        healthy = os.urandom(2 * BLOCK)
        delayed_path = incoming / 'delayed.bin'
        delayed_path.write_bytes(content)
        (incoming / 'healthy.bin').write_bytes(healthy)
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0,
                         MaxUpload=3500, SlotAllocation=100, MMapEnabled=0,
                         SharedFileReverifyDays=0, IncomingDir=incoming)
        notify_r, notify_w = os.pipe()
        release_r, release_w = os.pipe()
        arm = root / 'armed'
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8',
                   LD_PRELOAD=os.pathsep.join(filter(None, (os.getenv('AMULE_TEST_PRELOAD_RUNTIME'), fixture))), AMULE_TEST_READ_PATH=str(delayed_path),
                   AMULE_TEST_READ_ARM=str(arm), AMULE_TEST_READ_NOTIFY_FD=str(notify_w),
                   AMULE_TEST_READ_RELEASE_FD=str(release_r))
        peers = []
        ec = None
        proc = None
        with (root / 'console.log').open('w') as log:
            try:
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], env=env,
                                        stdout=log, stderr=log, pass_fds=(notify_w, release_r))
                os.close(notify_w)
                notify_w = None
                os.close(release_r)
                release_r = None
                ec = connect_daemon(proc, ec_port, root, peer_port)
                deadline = time.monotonic() + 30
                while True:
                    files = shared(ec)
                    if 'delayed.bin' in files and 'healthy.bin' in files:
                        break
                    assert time.monotonic() < deadline, 'shared files were not hashed'
                    time.sleep(0.05)
                # Both slots must exist before the first read snapshots the upload list.
                peers.append(Peer(peer_port, 0, files['delayed.bin']))
                peers.append(Peer(peer_port, 1, files['healthy.bin']))
                busy, other = peers
                # A completed probe proves the disk worker has already observed both
                # slots; an older snapshot containing only the busy slot cannot remain.
                other.request([(BLOCK, 2 * BLOCK)])
                other.receive((BLOCK, 2 * BLOCK), healthy)
                arm.touch()
                busy.request(ranges[:1])
                wait_read(notify_r, 0)
                # Populate both queues while the first read holds no upload-list lock.
                busy.request(ranges[1:])
                other.request([(0, BLOCK)])
                busy.barrier()
                other.barrier()
                for index in range(11):
                    os.write(release_w, b'R')
                    wait_read(notify_r, ranges[index + 1][0])
                    # The preceding block must arrive with the next disk read still held.
                    # Draining it also keeps this client's queue below its refill target.
                    busy.receive(ranges[index], content)
                # The busy client's twelfth read is now held. The other slot must have
                # been served in the first sweep, regardless of any request's size.
                other.receive((0, BLOCK), healthy)
                os.write(release_w, b'R')
                busy.receive(ranges[11], content)
                assert proc.poll() is None, 'daemon exited during storage delay test'
                print(f'{case}: prompt delivery, one read per client turn, other slot served; payloads verified')
            except BaseException:
                print((root / 'console.log').read_text()[-5000:])
                raise
            finally:
                # Release a gated syscall before stopping the daemon, including on failure.
                for fd in (release_w, release_r, notify_w):
                    if fd is not None:
                        os.close(fd)
                for peer in peers:
                    peer.close()
                if ec:
                    ec.sock.close()
                if proc:
                    stop_daemon(proc)
                os.close(notify_r)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('fixture')
    parser.add_argument('--case', choices=('small', 'full', 'large', 'mixed'))
    args = parser.parse_args()
    for case in ([args.case] if args.case else ['small', 'full', 'large', 'mixed']):
        retry_port_collisions(run, str(Path(args.binary).resolve()), str(Path(args.fixture).resolve()), case)
