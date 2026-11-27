#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A hole punch our server refuses moves its source to the next LowID-to-LowID step at once.

A fake ed2k-server gives the daemon a LowID, announces the LowID hole punch (0x20000) and
Lugdunum's callback (0x1000, 24-byte OP_IDCHANGE), and answers each source request with LowID
sources, so the daemon sends OP_LOWID_HOLEPUNCH_REQUEST for each. An OP_LOWID_HOLEPUNCH_FAIL for
one source moves that source on to the Lugdunum callback at once; it waits for the server's UDP
key, which this server never gives. A repeated refusal and one for a target we never asked about
change nothing. The other source moves on only when its punch times out.
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
OP_IDCHANGE = 0x40
OP_SERVERIDENT = 0x41
OP_FOUNDSOURCES = 0x42
OP_LOWID_HOLEPUNCH_REQUEST = 0x60
OP_LOWID_HOLEPUNCH_FAIL = 0x62
SRV_TCPFLG_LUGDUNUM_CALLBACK = 0x1000
SRV_TCPFLG_LOWID_HOLEPUNCH = 0x20000
SRV_UDPFLG_UDPOBFUSCATION = 0x200
ST_SERVERNAME = 0x01
ST_UDPFLAGS = 0x92
OUR_LOWID = 0x101
# ed2k-server's reason for a target with no UDP port.
FAIL_TARGET_NO_UDP = 4
# kHolePunchFallbackMs
FALLBACK_S = 60
# Lugdunum's callback is asked only for an odd LowID.
REFUSED = 0x10101
WAITING = 0x20203
NEVER_ASKED = 0x30305


def packet(opcode, body=b''):
    return struct.pack('<BIB', 0xe3, len(body) + 1, opcode) + body


def uint32_tag(name, value):
    return b'\x03' + struct.pack('<H', 1) + bytes([name]) + struct.pack('<I', value)


def string_tag(name, value):
    return b'\x02' + struct.pack('<H', 1) + bytes([name]) + struct.pack('<H', len(value)) + value


class FakeServer:
    """Logs the daemon in as a LowID and hands out the LowID sources set for each file."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.sources = {}
        self.punches = queue.Queue()
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
                    flags = SRV_TCPFLG_LOWID_HOLEPUNCH | SRV_TCPFLG_LUGDUNUM_CALLBACK
                    # ID, flags, aux port, reported IP, obfuscation port, the port we see.
                    self.peer.sendall(packet(OP_IDCHANGE, struct.pack('<6I', OUR_LOWID, flags, 0, 0, 0, 4662)))
                    tags = [string_tag(ST_SERVERNAME, b'Hole punch fixture'),
                            uint32_tag(ST_UDPFLAGS, SRV_UDPFLG_UDPOBFUSCATION)]
                    self.peer.sendall(packet(OP_SERVERIDENT, bytes(22) + struct.pack('<I', len(tags)) + b''.join(tags)))
                    self.logged_in.set()
                elif opcode in (OP_GETSOURCES, OP_GETSOURCES_OBFU):
                    ids = self.sources.get(body[:16], ())
                    records = b''.join(struct.pack('<IH', i, 4662) for i in ids)
                    self.peer.sendall(packet(OP_FOUNDSOURCES, body[:16] + bytes([len(ids)]) + records))
                elif opcode == OP_LOWID_HOLEPUNCH_REQUEST:
                    self.punches.put((struct.unpack('<I', body[:4])[0], time.monotonic()))
        except (EOFError, OSError):
            pass

    def refuse(self, target, reason=FAIL_TARGET_NO_UDP):
        self.peer.sendall(packet(OP_LOWID_HOLEPUNCH_FAIL, struct.pack('<IB', target, reason)))

    def wait_punches(self, ids, timeout=60):
        """The time each target's 0x60 arrived."""
        sent, deadline = {}, time.monotonic() + timeout
        while set(ids) - set(sent):
            target, at = self.punches.get(timeout=max(0.1, deadline - time.monotonic()))
            sent[target] = at
        return sent


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-holepunch-fail-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToED2K=1,
                         FilterLanIPs=0, LowIdHolePunch=1, LugdunumNatCallback=1,
                         VerboseDebug=1, VerboseDebugLogfile=1,
                         sections={'Debug': {'Cat_ED2k Client': 1, 'Cat_Servers': 1}})
        server = FakeServer()
        log = root / 'logfile'

        def logged(text):
            return log.read_text(errors='replace').count(text)

        def moved_on(target):
            return logged(f'Hole punch to source {target} failed, trying the Lugdunum callback')

        def unmatched(target):
            return logged(f'OP_LOWID_HOLEPUNCH_FAIL for source {target}, which no request of ours waits for')

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
                wait_log('the hole punch announcement',
                         lambda: logged('OP_IDCHANGE: server accepts LowID hole punch requests') > 0, 30)

                file_hash = os.urandom(16)
                server.sources[file_hash] = (REFUSED, WAITING)
                link = f'ed2k://|file|punch.bin|1048576|{file_hash.hex()}|/'
                assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']
                sent = server.wait_punches((REFUSED, WAITING))

                # Refused: that source moves on at once, the other keeps waiting.
                server.refuse(REFUSED)
                wait_log('the refused source to move on', lambda: moved_on(REFUSED) == 1, 10)
                assert time.monotonic() - sent[REFUSED] < FALLBACK_S / 2, 'the refused source waited for its timeout'
                assert moved_on(WAITING) == 0, 'the refusal moved the other source on too'

                # ed2k-server repeats its answer; a refusal for a target we never asked about is ignored.
                server.refuse(REFUSED)
                server.refuse(NEVER_ASKED)
                wait_log('the repeated refusal to be left unmatched', lambda: unmatched(REFUSED) == 1, 10)
                wait_log('the unknown target to be left unmatched', lambda: unmatched(NEVER_ASKED) == 1, 10)
                assert moved_on(REFUSED) == 1, 'a repeated refusal moved the source on again'

                # The other source moves on when its punch times out, as before.
                wait_log('the waiting source to time out', lambda: moved_on(WAITING) == 1, FALLBACK_S + 20)
                assert time.monotonic() - sent[WAITING] >= FALLBACK_S - 1, 'the waiting source moved on early'
                assert moved_on(REFUSED) == 1, 'the refused source moved on twice'
                assert server.punches.empty(), 'a source was punched again'
                assert proc.poll() is None, 'the daemon exited'
            except BaseException:
                print(log.read_text(errors='replace')[-8000:], file=sys.stderr)
                raise
    print('a refused hole punch moves its source on at once; the other source waits for its timeout')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
