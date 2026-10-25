#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""One address may hold at most 10 upload slots, even with free slots and nobody else waiting.

Ten sessions from address A take a slot each. An eleventh from A waits although slots are
free; a session from address B still gets a slot at once. When one of A's sessions ends, the
waiting one takes its slot. Linux uses 127.0.0.2 and 127.0.0.3; elsewhere pass two local
addresses with --addresses.
"""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

from AllSearchIntegrationTest import connect_daemon, exact, free_ports, retry_port_collisions, stop_daemon
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared

HARD_CAP = 10
OP_HELLOANSWER = 0x4c
OP_SETREQFILEID = 0x4f
OP_STARTUPLOADREQ = 0x54
OP_ACCEPTUPLOADREQ = 0x55
OP_CANCELTRANSFER = 0x56


def send(sock, opcode, data=b''):
    sock.sendall(struct.pack('<BIB', 0xe3, len(data) + 1, opcode) + data)


class Peer:
    def __init__(self, address, target, port, name, file_hash):
        self.sock = socket.socket()
        try:
            self.sock.bind((address, 0))
            self.sock.connect((target, port))
            nick = name.encode()
            tags = b'\x02\x01\x00\x01' + struct.pack('<H', len(nick)) + nick
            hello = (b'\x10' + hashlib.md5(nick).digest() + socket.inet_aton(address)
                     + struct.pack('<HI', self.sock.getsockname()[1], 1) + tags + bytes(6))
            send(self.sock, 0x01, hello)
            self.wait_for(OP_HELLOANSWER, 10)
            send(self.sock, OP_SETREQFILEID, file_hash)
            send(self.sock, OP_STARTUPLOADREQ, file_hash)
        except BaseException:
            self.sock.close()
            raise

    def wait_for(self, opcode, seconds):
        """True once the opcode arrives, False if it does not within the time."""
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self.sock.settimeout(remaining)
            try:
                _, size = struct.unpack('<BI', exact(self.sock, 5))
                if exact(self.sock, size)[0] == opcode:
                    return True
            except TimeoutError:
                return False

    def close(self):
        self.sock.close()


def run(binary, address_a, address_b):
    loopback = all(ipaddress.ip_address(a).is_loopback for a in (address_a, address_b))
    with tempfile.TemporaryDirectory(prefix='amule-upload-slot-scope-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        (root / 'Incoming' / 'shared.bin').write_bytes(os.urandom(1 << 20))
        ec_port, peer_port = free_ports(2, peer_index=1)
        # Unlimited upload with no traffic yet offers 20 slots, more than the cap.
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0, MaxUpload=0,
                         Address='127.0.0.1' if loopback else None)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        peers = []
        with (root / 'console.log').open('w') as log:
            proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], env=env,
                                    stdout=log, stderr=log)
            try:
                ec = connect_daemon(proc, ec_port, root, peer_port)
                deadline = time.monotonic() + 30
                while 'shared.bin' not in (files := shared(ec)):
                    assert time.monotonic() < deadline, 'shared file was not hashed'
                    time.sleep(0.1)
                file_hash = files['shared.bin']

                def open_peer(address, name):
                    target = '127.0.0.1' if loopback else address
                    peer = Peer(address, target, peer_port, name, file_hash)
                    peers.append(peer)
                    return peer

                for index in range(HARD_CAP):
                    peer = open_peer(address_a, f'scope-a-{index}')
                    assert peer.wait_for(OP_ACCEPTUPLOADREQ, 10), f'session {index} from A got no slot'
                # The empty-queue start runs only a second after the last slot started; within it the
                # session would be queued and capped by the best-client pick instead.
                time.sleep(1.5)
                extra = open_peer(address_a, 'scope-a-extra')
                assert not extra.wait_for(OP_ACCEPTUPLOADREQ, 4), 'A got more than 10 slots'
                other = open_peer(address_b, 'scope-b')
                assert other.wait_for(OP_ACCEPTUPLOADREQ, 10), 'B got no slot while slots were free'
                send(peers[0].sock, OP_CANCELTRANSFER)
                assert extra.wait_for(OP_ACCEPTUPLOADREQ, 10), 'the waiting session from A was not served'
                ec.sock.close()
            except BaseException:
                logfile = root / 'logfile'
                print(logfile.read_text(errors='replace')[-5000:] if logfile.exists() else 'no logfile')
                raise
            finally:
                for peer in peers:
                    peer.close()
                stop_daemon(proc)
    print(f'one address held at most {HARD_CAP} slots with free slots; the other address and the '
          'waiting session were served')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--addresses', nargs=2, default=('127.0.0.2', '127.0.0.3'),
                        help='two local addresses for the two peers')
    args = parser.parse_args()
    retry_port_collisions(run, str(Path(args.binary).resolve()), *args.addresses)
