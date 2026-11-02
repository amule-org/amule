#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Truncated block diagnostics must preserve wrong-file rejection on real peer sockets."""
from contextlib import contextmanager
import hashlib
from pathlib import Path
import socket
import struct
import sys
import tempfile
import zlib

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions, running_daemon,
                                      start_daemon)
from DaemonConfig import write_amule_conf
from DownloadDisplayRatesIntegrationTest import hello, local_ipv4
from ED2KLinkIntegrationTest import add_link
from ShareWatcherMoveIntegrationTest import tag_list
from UploadRefillIntegrationTest import packet, send


def start(binary, root, ports):
    return start_daemon(binary, root, ports)


def frame(peer, protocol, opcode, data):
    peer.sendall(struct.pack('<BIB', protocol, len(data) + 1, opcode) + data)


@contextmanager
def source(ec, address, peer_port, name):
    file_hash = hashlib.md5(name).digest()
    # Reserve a source identity on this host without accepting outgoing connections.
    with socket.socket() as identity, socket.socket() as peer:
        identity.bind((address, 0))
        source_port = identity.getsockname()[1]
        link = (f'ed2k://|file|{name.decode()}|8388608|{file_hash.hex()}|'
                f'sources,{address}:{source_port}|/')
        assert add_link(ec, link) == C['EC_OP_NOOP']
        peer.bind((address, 0))
        peer.settimeout(10)
        peer.connect((address, peer_port))
        greeting = hello(name, address, source_port)
        send(peer, 0x01, greeting)
        # Reach DS_DOWNLOADING: otherwise ProcessBlockPacket intentionally ignores data.
        while True:
            _, opcode, data = packet(peer)
            if opcode in (0x4c, 0x58):  # OP_HELLOANSWER / OP_REQUESTFILENAME
                send(peer, 0x59, file_hash + struct.pack('<H', len(name)) + name)
            elif opcode == 0x4f:  # OP_SETREQFILEID
                send(peer, 0x50, file_hash + bytes(2))
            elif opcode == 0x54:  # OP_STARTUPLOADREQ
                send(peer, 0x55)
            elif opcode == 0x47:  # OP_REQUESTPARTS
                assert data[:16] == file_hash
                offsets = struct.unpack_from('<6I', data, 16)
                requested = [(first, last) for first, last in zip(offsets[:3], offsets[3:])
                             if last > first]
                assert requested, 'source received no block request'
                yield peer, greeting, file_hash, requested[0][0]
                return


def block(file_hash, first, packed, large):
    content = b'x' * 1024
    offset = '<Q' if large else '<I'
    data = zlib.compress(content) if packed else content
    header = file_hash + struct.pack(offset, first)
    header += struct.pack('<I', len(data)) if packed else struct.pack(offset, first + len(data))
    return header, data


def hello_answer(peer, greeting):
    # A reply proves all preceding frames were handled while retaining the connection.
    send(peer, 0x01, greeting)
    while packet(peer)[1] != 0x4c:
        pass


def completed(ec, file_hash):
    # Keep repeated partfile tags instead of the EC fixture's dictionary view.
    payload = bytes([C['EC_OP_GET_DLOAD_QUEUE']]) + bytes(2)
    ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    flags, length = struct.unpack('!II', exact(ec.sock, 8))
    assert flags == 0x20
    reply = exact(ec.sock, length)
    tags, _ = tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])
    for name, _, children in tags:
        fields = {key: value for key, value, _ in children}
        if name == C['EC_TAG_PARTFILE'] and fields.get(C['EC_TAG_PARTFILE_HASH']) == file_hash:
            return int.from_bytes(fields[C['EC_TAG_PARTFILE_SIZE_DONE']], 'big')
    raise AssertionError('download missing from EC')


def check_layout(ec, address, peer_port, packed, large, wrong_length):
    protocol, opcode = ((0xc5, 0xa1 if packed else 0xa2) if large
                        else (0xc5, 0x40) if packed else (0xe3, 0x46))
    name = f'block-{packed}-{large}'.encode()
    with source(ec, address, peer_port, name) as (peer, greeting, file_hash, first):
        if wrong_length is not None:
            wrong_hash = bytes([file_hash[0] ^ 0xff]) + file_hash[1:]
            header, data = block(wrong_hash, first, packed, large)
            frame(peer, protocol, opcode, (header + data)[:wrong_length])
            try:
                send(peer, 0x01, greeting)
                while True:
                    assert packet(peer)[1] != 0x4c, (
                        'wrong file hash left the peer connected', packed, large, wrong_length)
            except (EOFError, ConnectionResetError, BrokenPipeError):
                pass
            return

        header, data = block(file_hash, first, packed, large)
        for length in range(len(header)):
            frame(peer, protocol, opcode, header[:length])
        hello_answer(peer, greeting)
        assert completed(ec, file_hash) == 0, 'truncated header wrote file data'
        # A complete valid packet must still be accepted after the short packets.
        frame(peer, protocol, opcode, header + data)
        hello_answer(peer, greeting)
        assert completed(ec, file_hash) == 1024, 'valid block after short headers was not downloaded'


def run_case(binary, packed, large, wrong_length):
    with tempfile.TemporaryDirectory(prefix='amule-short-block-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        address = local_ipv4()
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Address=address, Port=ports[1], UDPPort=ports[2],
                         FilterLanIPs=0, MaxDownload=0, MaxUpload=0)
        with running_daemon(binary, root, ports) as (proc, ec):
            check_layout(ec, address, ports[1], packed, large, wrong_length)
            assert proc.poll() is None, 'block packet killed the daemon'


def run(binary):
    for packed in (False, True):
        for large in (False, True):
            header, data = block(bytes(16), 0, packed, large)
            # Separate profiles keep wrong-file rejection from influencing subsequent
            # cases through source/IP tracking. Each peer must reach DS_DOWNLOADING.
            for wrong_length in (None, 16, len(header) - 1, len(header), len(header) + len(data)):
                run_case(binary, packed, large, wrong_length)
    print('PASS: all four block layouts, every truncated header, valid data, '
          'and wrong-file disconnects at four size boundaries')


if __name__ == '__main__':
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve())))
