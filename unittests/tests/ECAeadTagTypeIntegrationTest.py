#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A login whose encryption tags have the wrong type must not abort amuled.

GetTagData() asserts on a tag that is not raw bytes, and with --disable-fatal an assert
aborts. Each case sends one mistyped login request and checks that amuled answers and keeps
running.
"""
import hashlib, socket, struct, subprocess, sys, tempfile
from pathlib import Path
from AllSearchIntegrationTest import (C, EC, free_ports, log_snapshot, retry_port_collisions,
                                      stop_daemon, wait_daemon_started)
from DaemonConfig import EC_PASSWORD, daemon_env, write_amule_conf
from ECWire import authenticate  # Preflight before starting the daemon.

UINT8, STRING = 2, 6


def login(port, field_name, field_type):
    """Mutate one tag in an otherwise valid SRP request, reaching its type check."""
    def mistype(fields):
        return [field[:2] + bytes([field_type]) + field[3:]
                if struct.unpack_from('!H', field)[0] == C[field_name] * 2 else field
                for field in fields]
    with socket.create_connection(('127.0.0.1', port), timeout=30) as sock:
        _, op, fields = authenticate(sock, hashlib.md5(EC_PASSWORD.encode()).digest(),
                                    request_mutator=mistype)
        return op, fields


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-aead-tag-type-') as root:
        root = Path(root)
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0)
        with (root / 'stdout.log').open('w') as log:
            snapshot = log_snapshot(root / 'logfile')
            proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                    stdout=log, stderr=log, env=daemon_env(root))
            try:
                # Logging in first would hide what the mistyped logins do to a fresh daemon.
                wait_daemon_started(proc, ec_port, root, peer_port, snapshot=snapshot)

                cases = [
                    ('offer as UINT8', 'EC_TAG_CAN_AEAD', UINT8),
                    ('nonce as STRING', 'EC_TAG_AEAD_CLIENT_NONCE', STRING),
                    ('public key as STRING', 'EC_TAG_AEAD_CLIENT_PUBKEY', STRING),
                ]
                for name, field_name, field_type in cases:
                    op, reply = login(ec_port, field_name, field_type)
                    assert proc.poll() is None, f'{name}: daemon exited with {proc.returncode}'
                    message = reply.get(C['EC_TAG_STRING'], b'')
                    assert op == C['EC_OP_AUTH_FAIL'] and b'Invalid SRP handshake' in message, (name, op, reply)
                    print(f'PASS: {name} is refused and amuled keeps running')

                EC(ec_port, password=EC_PASSWORD).sock.close()
                print('PASS: a normal login still works')
            except BaseException:
                stop_daemon(proc, check=False)
                raise
            stop_daemon(proc)


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
