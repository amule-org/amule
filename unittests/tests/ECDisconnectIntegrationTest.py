#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Do not authenticate buffered packets after rejecting their EC connection."""
import hashlib
from pathlib import Path
import socket
import struct
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
import ECWire as W
from DaemonConfig import EC_PASSWORD, write_amule_conf


def packet(opcode, tags=(), flags=0x20):
    body = bytes([C[opcode]]) + struct.pack('!H', len(tags)) + b''.join(tags)
    return struct.pack('!II', flags, len(body)) + body


def authenticate(peer, **kwargs):
    return W.authenticate(peer, hashlib.md5(EC_PASSWORD.encode()).digest(),
                          capabilities=[W.tag('EC_TAG_CAN_NOTIFY')], **kwargs)


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-ec-disconnect-') as tmp:
        root = Path(tmp)
        for directory in ('Incoming', 'Temp'):
            (root / directory).mkdir()
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2])
        with running_daemon(binary, root, ports) as (proc, control):
            # Positive control: the very same notification-capable login succeeds
            # when no rejected packet precedes its proof.
            with socket.create_connection(('127.0.0.1', ports[0]), timeout=10) as peer:
                assert authenticate(peer)[1] == C['EC_OP_AUTH_OK']
            assert control.call(C['EC_OP_STAT_REQ'])[0] == C['EC_OP_STATS']
            granted = (root / 'logfile').read_text().count('Access granted.')

            with socket.create_connection(('127.0.0.1', ports[0]), timeout=10) as peer:
                # One write puts the encrypted proof and command behind a packet
                # whose version flags ReadPacket must reject. Buffered input must
                # not authenticate the connection after its asynchronous close.
                try:
                    authenticate(peer, proof_prefix=packet('EC_OP_STAT_REQ', flags=0x60),
                                 proof_followup=[packet('EC_OP_STAT_REQ')])
                except (EOFError, ConnectionResetError):
                    pass
                else:
                    raise AssertionError('dropped connection sent a reply')

            # This round trip is a main-loop barrier, and would also expose a
            # notifier registered by the dying socket. No sleeps are needed.
            assert control.call(C['EC_OP_STAT_REQ'])[0] == C['EC_OP_STATS']
            assert proc.poll() is None, 'rejected batch killed the daemon'
            assert (root / 'logfile').read_text().count('Access granted.') == granted, \
                'buffered proof authenticated a dropped connection'

            # A packet after AUTH_FAIL must also tear the socket down: a plain local
            # close raises no lost event, so OnLost would never destroy it.
            closed = (root / 'logfile').read_text().count('External connection closed.')
            with socket.create_connection(('127.0.0.1', ports[0]), timeout=10) as peer:
                encrypted, opcode, _ = authenticate(
                    peer,
                    proof_mutator=lambda _: W.packet('EC_OP_AUTH_PASSWD',
                                                      [W.tag('EC_TAG_SRP_M1', bytes(32))]))
                assert opcode == C['EC_OP_AUTH_FAIL']
                encrypted.sendall(packet('EC_OP_STAT_REQ'))
                try:
                    assert peer.recv(1) == b'', 'failed login stayed open'
                except ConnectionResetError:
                    pass
            assert control.call(C['EC_OP_STAT_REQ'])[0] == C['EC_OP_STATS']
            assert (root / 'logfile').read_text().count('External connection closed.') \
                == closed + 1, 'packet after AUTH_FAIL left the socket alive'
            print('PASS: rejected EC batch cannot authenticate or register a notifier')
            print('PASS: packet after AUTH_FAIL closes and releases the connection')
            print('PASS: ordinary notification login and independent EC client still work')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
