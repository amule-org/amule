#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise publication scheduling with an isolated daemon and loopback server."""
import argparse
import configparser
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time
import zlib

from OfferFilesInteropTest import C, connect, daemon, exact, shared, stop, wait_for
from OfferFilesInteropPeer import login_tags


def uint32_tag(name, value):
    raw = bytes([name]) if isinstance(name, int) else name.encode()
    return b'\x03' + struct.pack('<H', len(raw)) + raw + struct.pack('<I', value)


class Server:
    def __init__(self):
        # Reserve the ED2K UDP port (TCP + 4) along with the TCP listener.
        while True:
            self.tcp = socket.socket()
            self.tcp.bind(('127.0.0.1', 0))
            self.port = self.tcp.getsockname()[1]
            self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                self.udp.bind(('127.0.0.1', self.port + 4))
                break
            except OSError:
                self.tcp.close()
                self.udp.close()
        self.tcp.listen(1)
        self.peer = None
        self.destination = None
        self.soft = 2
        self.hard = 201
        self.offers = []
        self.errors = []
        self.stopping = False
        threading.Thread(target=self.serve, daemon=True).start()
        threading.Thread(target=self.ping, daemon=True).start()

    def limits(self, challenge=0):
        # Standard OP_GLOBSERVSTATRES; challenges are echoed for actual pings.
        self.udp.sendto(b'\xe3\x97' + struct.pack('<6I', challenge, 1, 0, 100,
                                               self.soft, self.hard), self.destination)

    def ping(self):
        try:
            while True:
                data, address = self.udp.recvfrom(4096)
                if data[:2] == b'\xe3\x96' and len(data) >= 6:
                    self.destination = address
                    self.limits(struct.unpack_from('<I', data, 2)[0])
        except OSError:
            pass

    def serve(self):
        try:
            self.peer, _ = self.tcp.accept()
            while True:
                protocol, length = struct.unpack('<BI', exact(self.peer, 5))
                body = exact(self.peer, length)
                if body[0] == 1:
                    fields = [('offerfiles_v', 1), ('offerfiles_batch_max', 2),
                              ('offerfiles_min_interval_ms', 500), (0x88, 6), (0x89, 201)]
                    if login_tags(body[1:]).get(b'offerfiles_v') == 1:
                        identity = (bytes(22) + struct.pack('<I', len(fields))
                                    + b''.join(uint32_tag(*field) for field in fields))
                    else:
                        name = b'Loopback publication fixture'
                        identity = (bytes(22) + struct.pack('<I', 1) + b'\x02\x01\x00\x01'
                                    + struct.pack('<H', len(name)) + name)
                    for opcode, payload in [(0x40, struct.pack('<II', 0x0100007f, 1)),
                                            (0x41, identity)]:
                        self.peer.sendall(struct.pack('<BIB', 0xe3, len(payload) + 1, opcode) + payload)
                elif body[0] == 0x15:
                    payload = zlib.decompress(body[1:]) if protocol == 0xd4 else body[1:]
                    self.offers.append(struct.unpack_from('<I', payload)[0])
                    if len(self.offers) == 1:
                        self.limits()
        except (OSError, EOFError) as error:
            if not self.stopping:
                self.errors.append(repr(error))
        except Exception as error:
            self.errors.append(repr(error))

    def close(self):
        self.stopping = True
        if self.peer:
            self.peer.close()
        self.tcp.close()
        self.udp.close()


def live_limits(binary):
    with tempfile.TemporaryDirectory(prefix='amule-offerfiles-live-limits-') as tmp:
        root = Path(tmp) / 'client'
        server = Server()
        process = stream = None
        try:
            process, stream, ec = daemon(binary, root, 6, 'live-limits', True,
                                         sections={'Obfuscation': {'IsClientCryptLayerSupported': 0}})
            config = configparser.ConfigParser()
            config.read(root / 'amule.conf')
            server.destination = ('127.0.0.1', config.getint('eMule', 'Port') + 3)
            connect(ec, server.port)
            wait_for(lambda: ec.call(C['EC_OP_GET_CONNSTATE'])[1][C['EC_TAG_CONNSTATE']][0] & 1,
                     timeout=15)
            wait_for(lambda: server.offers, timeout=15)
            def listed_soft_limit():
                _, tags = ec.call(C['EC_OP_GET_SERVER_LIST'])
                return tags[C['EC_TAG_SERVER']][1].get(C['EC_TAG_SERVER_FILES_SOFT'], (0, {}))[0]
            wait_for(lambda: listed_soft_limit() == 2, timeout=3)
            time.sleep(2)  # More than one publication interval; budget is exhausted.
            assert not server.errors, server.errors
            assert server.offers == [2], server.offers
            server.soft = 6
            server.limits()
            wait_for(lambda: sum(server.offers) == 6, timeout=5)
            assert server.offers == [2, 2, 2], server.offers
            assert not server.errors, server.errors
            print('Idle publication resumed after live UDP soft limit increased: PASS')
        except BaseException:
            if (root / 'logfile').exists():
                print((root / 'logfile').read_text(errors='replace')[-12000:])
            raise
        finally:
            server.close()
            if process:
                stop(process)
            if stream:
                stream.close()


def empty_library(binary):
    with tempfile.TemporaryDirectory(prefix='amule-offerfiles-empty-library-') as tmp:
        root = Path(tmp) / 'client'
        server = Server()
        process = stream = None
        try:
            process, stream, ec = daemon(binary, root, 0, 'empty-library', False)
            config = configparser.ConfigParser()
            config.read(root / 'amule.conf')
            server.destination = ('127.0.0.1', config.getint('eMule', 'Port') + 3)
            connect(ec, server.port)
            wait_for(lambda: ec.call(C['EC_OP_GET_CONNSTATE'])[1][C['EC_TAG_CONNSTATE']][0] & 1,
                     timeout=15)
            # Allow several one-second maintenance ticks on the empty library.
            time.sleep(3)
            assert not server.offers, server.offers
            (root / 'Incoming' / 'new-file.bin').write_bytes(b'publication timer regression' * 3)
            assert ec.call(C['EC_OP_SHAREDFILES_RELOAD'])[0] == C['EC_OP_NOOP']
            wait_for(lambda: 'new-file.bin' in shared(ec), timeout=30)
            wait_for(lambda: server.offers, timeout=5)
            assert server.offers == [1], server.offers
            assert not server.errors, server.errors
            print('Empty legacy publication did not postpone the first real offer: PASS')
        except BaseException:
            if (root / 'logfile').exists():
                print((root / 'logfile').read_text(errors='replace')[-12000:])
            raise
        finally:
            server.close()
            if process:
                stop(process)
            if stream:
                stream.close()


def known_file_after_idle(binary, enabled):
    with tempfile.TemporaryDirectory(prefix='amule-offerfiles-known-file-') as tmp:
        root = Path(tmp) / 'client'
        server = Server()
        process = stream = None
        parked = root / 'parked.bin'
        original = None

        def prepare(client_root):
            nonlocal original
            original = next((client_root / 'Incoming').iterdir())
            original.rename(parked)

        try:
            process, stream, ec = daemon(binary, root, 2, 'known-file', enabled, prepare,
                                         sections={'Debug': {'Cat_KnownFileList': 1}})
            config = configparser.ConfigParser()
            config.read(root / 'amule.conf')
            server.destination = ('127.0.0.1', config.getint('eMule', 'Port') + 3)
            connect(ec, server.port)
            wait_for(lambda: ec.call(C['EC_OP_GET_CONNSTATE'])[1][C['EC_TAG_CONNSTATE']][0] & 1,
                     timeout=15)
            wait_for(lambda: server.offers, timeout=15)
            # Drain the initial library, then let a further eligible tick mark
            # publication idle. Legacy scheduling needs its full minute here.
            time.sleep(3 if enabled else 62)
            assert server.offers == [1], server.offers
            # Restore the original name and mtime: this must take the known-file
            # path, without an asynchronous hash job waking the publisher.
            parked.rename(original)
            assert ec.call(C['EC_OP_SHAREDFILES_RELOAD'])[0] == C['EC_OP_NOOP']
            wait_for(lambda: original.name in shared(ec), timeout=10)
            text = (root / 'logfile').read_text(errors='replace')
            # Release binaries omit debug diagnostics; Debug also proves that
            # this exercised the known-file route rather than rehashing.
            if 'KnownFileList:' in text:
                assert f"Added known file '{original.name}' to shares" in text, text[-4000:]
            wait_for(lambda: len(server.offers) == 2, timeout=5)
            assert server.offers == [1, 1], server.offers
            assert not server.errors, server.errors
            print(f'Known file woke idle publication (v1={enabled}): PASS')
        except BaseException:
            if (root / 'logfile').exists():
                print((root / 'logfile').read_text(errors='replace')[-12000:])
            raise
        finally:
            server.close()
            if process:
                stop(process)
            if stream:
                stream.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('daemon')
    parser.add_argument('--legacy-only', action='store_true')
    args = parser.parse_args()
    if not args.legacy_only:
        live_limits(args.daemon)
        known_file_after_idle(args.daemon, True)
    empty_library(args.daemon)
    known_file_after_idle(args.daemon, False)
