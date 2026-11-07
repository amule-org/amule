#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source replies must survive real request framing, file lifecycles and version negotiation."""
from contextlib import ExitStack
import hashlib
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time
import zlib

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon, wait_for
from CancelCompletedIntegrationTest import md4, part_met
from DaemonConfig import write_amule_conf
from ED2KLinkIntegrationTest import add_link
from ShareWatcherMoveIntegrationTest import wait_shared
from UploadRefillIntegrationTest import packet

PARTSIZE = 9728000


def local_address():
    # UDP connect selects an interface without sending a datagram. Download sources reject
    # loopback addresses even with FilterLanIPs=0; every fixture peer still lives on this host.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(('239.255.255.250', 1900))
            address = probe.getsockname()[0]
        except OSError:
            address = socket.gethostbyname(socket.gethostname())
    if address.startswith('127.'):
        print('SKIP: no local non-loopback IPv4 address for source discovery')
        sys.exit(77)
    return address


def send(peer, protocol, opcode, data=b''):
    peer.sendall(struct.pack('<BIB', protocol, len(data) + 1, opcode) + data)


def receive(peer):
    protocol, opcode, data = packet(peer)
    return opcode, zlib.decompress(data) if protocol == 0xd4 else data


class Peer:
    def __init__(self, stack, address, daemon_port, name, sx1=1, sx2=False, extended=True,
                 identity_port=None):
        self.sock = stack.enter_context(socket.socket())
        self.sock.bind((address, 0))
        self.sock.settimeout(10)
        self.sock.connect((address, daemon_port))
        self.port = identity_port or self.sock.getsockname()[1]
        self.user_hash = hashlib.md5(name.encode()).digest()
        tags = b''.join(b'\x03\x01\x00' + bytes([key]) + struct.pack('<I', value)
                        for key, value in ((0xfb, 50 << 10),
                                           (0xfa, (sx1 << 12) | (int(extended) << 8)),
                                           (0xfe, int(sx2) << 10)))
        self.hello = (b'\x10' + self.user_hash + socket.inet_aton(address)
                      + struct.pack('<HI', self.port, 3) + tags + bytes(6))
        self.extended = extended
        send(self.sock, 0xe3, 0x01, self.hello)
        self.until(0x4c)

    def until(self, wanted):
        deadline = time.monotonic() + 10
        while True:
            self.sock.settimeout(max(0.01, deadline - time.monotonic()))
            opcode, data = receive(self.sock)
            if opcode == wanted:
                return data
            assert time.monotonic() < deadline, f'no reply opcode {wanted:#x}'

    def file_request(self, file_hash, parts, mask):
        status = struct.pack('<H', parts) + bytes([mask]) if self.extended else b''
        return file_hash + status

    def prepare(self, file_hash, parts, mask):
        send(self.sock, 0xe3, 0x58, self.file_request(file_hash, parts, mask))
        assert self.until(0x59)[:16] == file_hash


def seed_upload(stack, address, daemon_port, file_hash, name):
    peer = Peer(stack, address, daemon_port, name)
    peer.prepare(file_hash, 1, 1)
    send(peer.sock, 0xe3, 0x54, file_hash)
    peer.until(0x55)
    return peer


def seed_download(stack, address, daemon_port, ec):
    identity = stack.enter_context(socket.socket())
    identity.bind((address, 0))
    # No file data is transferred: the valid hashset is needed only to reach DS_ONQUEUE.
    hashes = bytes(range(32))
    file_hash = md4(hashes)
    parts = ':'.join(hashes[i:i + 16].hex() for i in (0, 16))
    link = (f'ed2k://|file|partial.bin|{PARTSIZE + 1}|{file_hash.hex()}|p={parts}|'
            f'sources,{address}:{identity.getsockname()[1]}|/')
    assert add_link(ec, link) == C['EC_OP_NOOP']
    peer = Peer(stack, address, daemon_port, 'download-source', sx1=0,
                identity_port=identity.getsockname()[1])
    name = b'partial.bin'
    send(peer.sock, 0xe3, 0x59, file_hash + struct.pack('<H', len(name)) + name)
    send(peer.sock, 0xe3, 0x50, file_hash + bytes(2))
    deadline = time.monotonic() + 10
    while True:
        peer.sock.settimeout(max(0.01, deadline - time.monotonic()))
        opcode, data = receive(peer.sock)
        if opcode == 0x58:
            send(peer.sock, 0xe3, 0x59, file_hash + struct.pack('<H', len(name)) + name)
            send(peer.sock, 0xe3, 0x50, file_hash + bytes(2))
        elif opcode == 0x51:
            send(peer.sock, 0xe3, 0x52, file_hash + b'\x02\x00' + hashes)
        elif opcode == 0x54:
            # SendStartupLoadReq has put this source on queue; it never delivers file data.
            return file_hash, peer
        assert time.monotonic() < deadline, 'source did not reach the download queue'


def query(peer, file_hash, parts, mask, mode, requested):
    request = bytes([requested, 0, 0]) if requested else b''
    opcode = 0x83 if requested else 0x81
    if mode == 'standalone':
        send(peer.sock, 0xc5, opcode, request + file_hash)
        # A filename reply on this socket proves the source request was handled, even when
        # no source qualifies. Silence or a timeout is never treated as a successful negative.
        send(peer.sock, 0xe3, 0x58, peer.file_request(file_hash, parts, mask))
        barrier = 0x59
    else:
        size = struct.pack('<Q', PARTSIZE + 1) if mode == 'extended' else b''
        body = file_hash + size + bytes([opcode]) + request + b'\x58'
        body += peer.file_request(file_hash, parts, mask)[16:]
        send(peer.sock, 0xc5, 0xa4 if mode == 'extended' else 0x92, body)
        barrier = 0x93
    replies = []
    deadline = time.monotonic() + 10
    while True:
        peer.sock.settimeout(max(0.01, deadline - time.monotonic()))
        opcode, data = receive(peer.sock)
        if opcode == barrier:
            assert data[:16] == file_hash, 'barrier belongs to a different file'
            return replies
        if opcode in (0x82, 0x84):
            replies.append((opcode, data))
        assert time.monotonic() < deadline, 'no source-request barrier reply'


def check_reply(replies, file_hash, source, address, version, sx2):
    assert len(replies) == 1, ('expected one source reply', replies)
    opcode, data = replies[0]
    expected = (bytes([version]) if sx2 else b'') + file_hash + b'\x01\x00'
    ip = socket.inet_aton(address)
    expected += (ip[::-1] if version >= 3 else ip) + struct.pack('<H', source.port) + bytes(6)
    if version >= 2:
        expected += source.user_hash
    if version >= 4:
        expected += b'\x00'
    assert opcode == (0x84 if sx2 else 0x82), (opcode, sx2)
    assert data == expected, ('wrong source wire bytes', data.hex(), expected.hex())


def run(binary):
    address = local_address()
    with tempfile.TemporaryDirectory(prefix='amule-source-exchange-') as directory, ExitStack() as stack:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        content = b'completed download' * 1024
        completed_hash = md4(content)
        (root / 'Temp/001.part').write_bytes(content)
        (root / 'Temp/001.part.met').write_bytes(part_met(completed_hash, 'completed.bin', len(content)))
        (root / 'Incoming/shared.bin').write_bytes(b'shared file' * 1024)
        ports = free_ports(2, peer_index=1)
        write_amule_conf(root, ports[0], Address=address, Port=ports[1], UDPEnable=0,
                         FilterLanIPs=0, MaxUpload=0, MaxConnections=500)
        proc, ec = stack.enter_context(running_daemon(binary, root, ports))
        files = wait_shared(proc, ec, ['shared.bin', 'completed.bin'], 30)
        wait_for(proc, 'completed download data', lambda: (root / 'Incoming/completed.bin').exists())
        assert (root / 'Incoming/completed.bin').read_bytes() == content
        cases = []
        for name in ('shared.bin', 'completed.bin'):
            source = seed_upload(stack, address, ports[1], files[name], name)
            cases.append((name, files[name], 1, source))
        partial_hash, source = seed_download(stack, address, ports[1], ec)
        cases.append(('partial.bin', partial_hash, 2, source))
        count = 0
        for name, file_hash, parts, source in cases:
            for mode in ('standalone', 'multipacket', 'extended'):
                # Extended multipackets validate the actual file size before reaching the builder.
                if mode == 'extended' and name != 'partial.bin':
                    continue
                for sx1, requested in ((1, 0), (2, 0), (3, 0), (4, 0),
                                       (4, 1), (1, 3), (1, 4), (4, 255)):
                    with ExitStack() as clients:
                        peer = Peer(clients, address, ports[1], f'query-{count}', sx1, bool(requested))
                        peer.prepare(file_hash, parts, 0)
                        replies = query(peer, file_hash, parts, 0, mode, requested)
                        check_reply(replies, file_hash, source, address,
                                    min(requested, 4) if requested else sx1, bool(requested))
                        count += 1
                for extended, mask in ((True, (1 << parts) - 1), (False, 0)):
                    with ExitStack() as clients:
                        peer = Peer(clients, address, ports[1], f'filter-{count}', 1, True, extended)
                        peer.prepare(file_hash, parts, mask)
                        replies = query(peer, file_hash, parts, mask, mode, 4)
                        if extended:
                            assert not replies, 'sent a source with no needed parts'
                        else:
                            check_reply(replies, file_hash, source, address, 4, True)
                        count += 1
        # Exact standalone framing is checked before any file lookup or reply is attempted.
        for opcode, valid in ((0x81, partial_hash), (0x83, b'\x04\x00\x00' + partial_hash)):
            for bad in (valid[:-1], valid + b'\x00'):
                with ExitStack() as clients:
                    peer = Peer(clients, address, ports[1], f'bad-{count}', 1, True)
                    send(peer.sock, 0xc5, opcode, bad)
                    try:
                        assert peer.sock.recv(1) == b'', 'malformed source request stayed connected'
                    except ConnectionResetError:
                        pass
                    count += 1
        assert proc.poll() is None, 'source exchange killed the daemon'
        print(f'PASS: {count} real-socket source-exchange cases, including a completed CPartFile')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
