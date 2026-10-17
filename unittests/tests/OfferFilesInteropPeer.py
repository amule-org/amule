#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Loopback search observer and download source for OFFERFILES regressions."""
import ctypes
import ctypes.util
import socket
import struct
import threading
import zlib

from AllSearchIntegrationTest import exact


def md4(data):
    crypto = ctypes.CDLL(ctypes.util.find_library('crypto'))
    crypto.MD4.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)
    digest = (ctypes.c_ubyte * 16)()
    crypto.MD4(data, len(data), digest)
    return bytes(digest)


def send(sock, opcode, payload=b''):
    sock.sendall(struct.pack('<BIB', 0xe3, len(payload) + 1, opcode) + payload)


def receive(sock, with_protocol=False):
    protocol, length = struct.unpack('<BI', exact(sock, 5))
    assert protocol in (0xe3, 0xd4, 0xc5) and 0 < length <= 1_000_000, (protocol, length)
    body = exact(sock, length)
    decoded = (body[0], zlib.decompress(body[1:]) if protocol == 0xd4 else body[1:])
    return (protocol, *decoded) if with_protocol else decoded


def tags(data, offset, count):
    result = {}
    for _ in range(count):
        kind = data[offset]
        offset += 1
        if kind & 0x80:
            name = data[offset:offset + 1]
            offset += 1
            kind &= 0x7f
        else:
            length = struct.unpack_from('<H', data, offset)[0]
            offset += 2
            name = data[offset:offset + length]
            offset += length
        if kind == 2 or 0x11 <= kind <= 0x20:
            if kind == 2:
                length = struct.unpack_from('<H', data, offset)[0]
                offset += 2
            else:
                length = kind - 0x10
            value = data[offset:offset + length].decode('utf-8')
        else:
            length = {3: 4, 8: 2, 9: 1, 11: 8}[kind]
            value = int.from_bytes(data[offset:offset + length], 'little')
        offset += length
        assert name not in result, name
        result[name] = value
    return result, offset


class SearchObserver:
    """Read names and complete-source counts from the server's real search path."""
    nick = 'interop-search-observer'

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        name = self.nick.encode()
        login = (b'\x77' * 16 + struct.pack('<IHI', 0, 0, 1)
                 + b'\x82\x01' + struct.pack('<H', len(name)) + name)
        send(self.sock, 0x01, login)
        while receive(self.sock)[0] != 0x41:  # OP_SERVERIDENT
            pass

    def search(self, term):
        term = term.encode()
        send(self.sock, 0x16, b'\x01' + struct.pack('<H', len(term)) + term)
        while True:
            opcode, data = receive(self.sock)
            if opcode == 0x33:  # OP_SEARCHRESULT
                break
        count = struct.unpack_from('<I', data)[0]
        offset, rows = 4, {}
        for _ in range(count):
            file_hash = data[offset:offset + 16]
            n = struct.unpack_from('<I', data, offset + 22)[0]
            fields, offset = tags(data, offset + 26, n)
            rows[file_hash] = fields
        assert data[offset:] == b'\x00', 'fixture unexpectedly needs pagination'
        return rows

    def close(self):
        self.sock.close()


class CompletionSource:
    """Seed one complete part; deliver the missing tail over a real ED2K socket."""
    name = 'Linux-completion-fixture.bin'
    part_size = 9728000

    def __init__(self, root):
        self.data = b'\x5a' * self.part_size + b'completion-tail' * 8
        self.hashes = md4(self.data[:self.part_size]) + md4(self.data[self.part_size:])
        self.file_hash = md4(self.hashes)
        temp = root / 'Temp'
        temp.mkdir()
        part = temp / '001.part'
        part.write_bytes(self.data[:self.part_size])
        name = self.name.encode()
        fields = [b'\x82\x01' + struct.pack('<H', len(name)) + name,
                  b'\x83\x02' + struct.pack('<I', len(self.data))]
        for mark, value in ((9, self.part_size), (10, len(self.data))):
            fields.append(b'\x03\x02\x00' + bytes([mark]) + b'0' + struct.pack('<I', value))
        (temp / '001.part.met').write_bytes(
            b'\xe0' + struct.pack('<I', int(part.stat().st_mtime)) + self.file_hash
            + struct.pack('<H', 2) + self.hashes + struct.pack('<I', len(fields)) + b''.join(fields))
        self.sock = None
        self.errors = []
        self.thread = None

    def start(self, port):
        # An incoming LowID peer can exchange a file it also downloads. This
        # uses the normal known-source path, without weakening the production
        # filter which rejects loopback IPs in outbound source lists.
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
        self.port = self.sock.getsockname()[1]
        send(self.sock, 0x01, b'\x10' + b'\x66' * 16 + struct.pack('<IHIIH', 42, self.port, 0, 0, 0))
        assert receive(self.sock)[0] == 0x4c, 'expected peer HELLOANSWER'
        send(self.sock, 0x58, self.file_hash)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        try:
            while True:
                protocol, opcode, payload = receive(self.sock, with_protocol=True)
                if protocol == 0xc5:
                    continue
                if opcode == 0x01:  # OP_HELLO, no eMule extension capabilities
                    send(self.sock, 0x4c, b'\x66' * 16 + struct.pack('<IHIIH', 0x0100007f, self.port, 0, 0, 0))
                elif opcode == 0x58:  # OP_REQUESTFILENAME
                    name = self.name.encode()
                    send(self.sock, 0x59, self.file_hash + struct.pack('<H', len(name)) + name)
                elif opcode == 0x59:  # The reciprocal request made us a known source.
                    send(self.sock, 0x50, self.file_hash + struct.pack('<H', 2) + b'\x03')
                elif opcode == 0x4f:  # OP_SETREQFILEID
                    send(self.sock, 0x50, self.file_hash + struct.pack('<H', 2) + b'\x03')
                elif opcode == 0x51:  # OP_HASHSETREQUEST
                    send(self.sock, 0x52, self.file_hash + struct.pack('<H', 2) + self.hashes)
                elif opcode == 0x54:  # OP_STARTUPLOADREQ
                    send(self.sock, 0x55)
                elif opcode == 0x47:  # OP_REQUESTPARTS
                    assert payload[:16] == self.file_hash, payload
                    ranges = struct.unpack_from('<6I', payload, 16)
                    for start, end in zip(ranges[:3], ranges[3:]):
                        if end > start:
                            assert self.part_size <= start < end <= len(self.data), ranges
                            send(self.sock, 0x46, self.file_hash + struct.pack('<II', start, end) + self.data[start:end])
        except (OSError, EOFError):
            pass
        except Exception as error:
            self.errors.append(repr(error))

    def close(self):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
        if self.thread:
            self.thread.join(timeout=1)
