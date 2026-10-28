#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A login whose encryption tags have the wrong type must not abort amuled.

GetTagData() asserts on a tag that is not raw bytes, and with --disable-fatal an assert
aborts. Each case sends one mistyped login request and checks that amuled answers and keeps
running.
"""
import hashlib, os, socket, struct, subprocess, sys, tempfile, time
from pathlib import Path
from AllSearchIntegrationTest import (C, EC, PortCollision, exact, free_ports, log_snapshot,
                                      new_log_text, parse_tags, retry_port_collisions,
                                      stop_daemon, string, tag)
from DaemonConfig import EC_PASSWORD, write_amule_conf

UINT8, STRING, HASH16 = 2, 6, 9
KEY = bytes(range(32))  # X25519_KEY_LEN; any 32 bytes reach the type check
NONCE = bytes(range(32, 64))  # NONCE_TAG_LEN
CHACHA20 = bytes([2])


def call(sock, op, tags):
    payload = bytes([op]) + struct.pack('!H', len(tags)) + b''.join(tags)
    sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    _, length = struct.unpack('!II', exact(sock, 8))
    reply = exact(sock, length)
    return reply[0], parse_tags(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]


def login(port, offer, nonce, pubkey):
    """Returns the reply to the password, or None when the request got no salt."""
    with socket.create_connection(('127.0.0.1', port), timeout=30) as sock:
        op, tags = call(sock, C['EC_OP_AUTH_REQ'], [
            string(C['EC_TAG_CLIENT_NAME'], 'tag type regression'),
            string(C['EC_TAG_CLIENT_VERSION'], '1'),
            tag(C['EC_TAG_PROTOCOL_VERSION'], struct.pack('!H', C['EC_CURRENT_PROTOCOL_VERSION']), 3),
            offer, nonce, pubkey])
        if op != C['EC_OP_AUTH_SALT']:
            return op, tags
        salt = tags[C['EC_TAG_PASSWD_SALT']][0]
        proof = hashlib.md5((hashlib.md5(EC_PASSWORD.encode()).hexdigest()
                             + hashlib.md5(f'{salt:X}'.encode()).hexdigest()).encode()).digest()
        return call(sock, C['EC_OP_AUTH_PASSWD'], [tag(C['EC_TAG_PASSWD_HASH'], proof, HASH16)])


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-aead-tag-type-') as root:
        root = Path(root)
        ec_port = free_ports(1)[0]
        write_amule_conf(root, ec_port)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        with (root / 'stdout.log').open('w') as log:
            snapshot = log_snapshot(root / 'logfile')
            proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                    stdout=log, stderr=log, env=env)
            try:
                while True:
                    if proc.poll() is not None:
                        raise AssertionError(f'daemon exited before EC was up: {proc.returncode}')
                    startup = new_log_text(root / 'logfile', snapshot)
                    if 'Could not listen for external connections at ' in startup:
                        raise PortCollision(f'Daemon could not bind EC port {ec_port}')
                    if 'amuled: OnInit - starting timer' in startup:
                        break
                    time.sleep(0.1)

                offer, nonce, pubkey = (tag(C['EC_TAG_CAN_AEAD'], CHACHA20),
                                        tag(C['EC_TAG_AEAD_CLIENT_NONCE'], NONCE),
                                        tag(C['EC_TAG_AEAD_CLIENT_PUBKEY'], KEY))
                cases = [
                    ('offer as UINT8', (tag(C['EC_TAG_CAN_AEAD'], CHACHA20, UINT8), nonce, pubkey)),
                    ('nonce as STRING', (offer, tag(C['EC_TAG_AEAD_CLIENT_NONCE'], NONCE, STRING), pubkey)),
                    ('public key as STRING', (offer, nonce, tag(C['EC_TAG_AEAD_CLIENT_PUBKEY'], KEY, STRING))),
                ]
                for name, tags in cases:
                    op, reply = login(ec_port, *tags)
                    assert proc.poll() is None, f'{name}: daemon exited with {proc.returncode}'
                    if name == 'public key as STRING':
                        # A mistyped key counts as missing: the login is refused, not run in clear.
                        message = reply.get(C['EC_TAG_STRING'], (b'',))[0]
                        assert op == C['EC_OP_AUTH_FAIL'] and b'no usable key' in message, (name, op, reply)
                    print(f'PASS: {name} is answered and amuled keeps running')

                EC(ec_port, password=EC_PASSWORD)
                print('PASS: a normal login still works')
            finally:
                if proc.poll() is None:
                    stop_daemon(proc)


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
