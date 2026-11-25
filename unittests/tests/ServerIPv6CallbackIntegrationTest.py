#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A LowID requester asks its server for the IPv6 callback only where it can work.

A fake server gives the daemon a LowID, offers the IPv6 callback (ST_IPV6_CALLBACK) and reports
our IPv6 as reached by its dial-back (ST_IPV6_STATUS 0x07). The daemon then asks for each LowID
source with OP_IPV6_CALLBACK_REQUEST. A refusal, or no answer, sends the source on
LowID-to-LowID. Once the offer is withdrawn, a LowID source is not kept or asked. An
OP_CALLBACKREQUESTED_IPV6 for loopback is not dialled.
"""
import os
from pathlib import Path
import queue
import socket
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions,
                                      running_daemon, string)
from DaemonConfig import write_amule_conf

OP_LOGINREQUEST = 0x01
OP_GETSOURCES = 0x19
OP_GETSOURCES_OBFU = 0x23
OP_CALLBACKREQUESTED_IPV6 = 0x26
OP_CALLBACK_FAIL = 0x36
OP_IDCHANGE = 0x40
OP_SERVERIDENT = 0x41
OP_FOUNDSOURCES = 0x42
OP_IPV6_CALLBACK_REQUEST = 0x64
ST_IPV6_CALLBACK = 0xaa
ST_IPV6_STATUS = 0xab
OUR_LOWID = 0x100
ANSWER_S = 10  # kIPv6CallbackAnswerMs


def packet(opcode, body=b''):
    return struct.pack('<BIB', 0xe3, len(body) + 1, opcode) + body


def uint32_tag(name, value):
    return b'\x03' + struct.pack('<H', 1) + bytes([name]) + struct.pack('<I', value)


def ident(offer):
    tags = [uint32_tag(ST_IPV6_STATUS, 0x07)] + ([uint32_tag(ST_IPV6_CALLBACK, 1)] if offer else [])
    return packet(OP_SERVERIDENT, bytes(22) + struct.pack('<I', len(tags)) + b''.join(tags))


class FakeServer:
    """Logs the daemon in as a LowID and hands out the LowID sources set for each file."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.sources = {}
        self.requests = queue.Queue()
        self.peer = None
        self.logged_in = threading.Event()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        try:
            self.peer = self.sock.accept()[0]
            while True:
                _, length = struct.unpack('<BI', exact(self.peer, 5))
                body = exact(self.peer, length)
                opcode, body = body[0], body[1:]
                if opcode == OP_LOGINREQUEST:
                    self.peer.sendall(packet(OP_IDCHANGE, struct.pack('<II', OUR_LOWID, 0)) + ident(True))
                    self.logged_in.set()
                elif opcode in (OP_GETSOURCES, OP_GETSOURCES_OBFU):
                    ids = self.sources.get(body[:16], ())
                    records = b''.join(struct.pack('<IH', i, 4662) for i in ids)
                    self.peer.sendall(packet(OP_FOUNDSOURCES, body[:16] + bytes([len(ids)]) + records))
                elif opcode == OP_IPV6_CALLBACK_REQUEST:
                    self.requests.put(struct.unpack('<I', body[:4])[0])
        except (EOFError, OSError):
            pass

    def send(self, data):
        self.peer.sendall(data)

    def wait_request(self, target, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            got = self.requests.get(timeout=max(0.1, deadline - time.monotonic()))
            if got == target:
                return
            raise AssertionError(f'asked for {got:#x}, expected {target:#x}')


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-ipv6-callback-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToED2K=1,
                         FilterLanIPs=0, VerboseDebug=1, VerboseDebugLogfile=1,
                         sections={'Debug': {'Cat_ED2k Client': 1, 'Cat_Servers': 1}})
        server = FakeServer()
        log = root / 'logfile'

        def logged(text):
            return log.read_text(errors='replace').count(text)

        def wait_log(what, check, timeout):
            deadline = time.monotonic() + timeout
            while not check():
                assert time.monotonic() < deadline, f'timed out waiting for {what}'
                time.sleep(0.2)

        with running_daemon(binary, root, ports, console='stdout.log') as (proc, ec):
            try:
                assert ec.call(C['EC_OP_SERVER_ADD'], [
                    string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{server.port}')])[0] == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                assert server.logged_in.wait(30), 'the daemon did not log in'
                wait_log('the LowID login', lambda: logged('with LowID') > 0, 30)

                def download(name, ids):
                    file_hash = os.urandom(16)
                    server.sources[file_hash] = ids
                    link = f'ed2k://|file|{name}.bin|1048576|{file_hash.hex()}|/'
                    assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']

                # Refused: the source goes on LowID-to-LowID at once.
                download('refused', (0x10101,))
                server.wait_request(0x10101)
                server.send(packet(OP_CALLBACK_FAIL))
                wait_log('the refusal to move the source on',
                         lambda: logged(f'IPv6 callback to source {0x10101} refused') == 1, 10)

                # Unanswered: the same after the answer window.
                sent = time.monotonic()
                download('unanswered', (0x20202,))
                server.wait_request(0x20202)
                wait_log('the window to pass',
                         lambda: logged(f'IPv6 callback to source {0x20202} unanswered') == 1, ANSWER_S + 15)
                assert time.monotonic() - sent >= ANSWER_S - 1, 'the window ended early'

                # Withdrawn: a LowID source is no longer kept, so nothing is asked.
                server.send(ident(False))
                time.sleep(1)
                download('withdrawn', (0x30303,))
                time.sleep(5)
                assert server.requests.empty(), 'asked for the IPv6 callback after the offer was withdrawn'

                # Loopback is never dialled.
                try:
                    listener = socket.socket(socket.AF_INET6)
                    listener.bind(('::1', 0))
                except OSError:
                    listener = None
                    print('no IPv6 loopback here: the loopback request is not checked')
                if listener is not None:
                    listener.listen(1)
                    listener.settimeout(3)
                    port = listener.getsockname()[1]
                    server.send(packet(OP_CALLBACKREQUESTED_IPV6,
                                       socket.inet_pton(socket.AF_INET6, '::1') + struct.pack('<H', port)))
                    wait_log('the loopback request to be ignored',
                             lambda: logged(f'Ignored IPv6 callback request to [::1]:{port}') == 1, 10)
                    try:
                        listener.accept()
                        raise AssertionError('the daemon dialled loopback')
                    except socket.timeout:
                        pass
                    finally:
                        listener.close()
                assert proc.poll() is None, 'the daemon exited'
            except BaseException:
                print(log.read_text(errors='replace')[-8000:], file=sys.stderr)
                raise
    print('the IPv6 callback is asked only where it can work, and a failure moves the source on')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
