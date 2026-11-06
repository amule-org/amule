#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Kad requests log whether their receiver key proves the sender receives at its IP.

A plain and a keyed source publish each log their own result, and both are still indexed. Two is all
the per-IP limit lets through in a minute. The other requests share the logging, so one keyed packet
each is enough, and the callback limit allows only one.
"""
import hashlib
import os
import socket
import struct
import sys
import tempfile
from pathlib import Path

from AllSearchIntegrationTest import retry_port_collisions, wait_for
from KadSourceTagsECTest import KADEMLIA2_PUBLISH_SOURCE_REQ, core_tags, kad_id, running_kad_daemon, search

UDP_KEY_SECRET = 0x5eed1234
MAGIC_UDP_SYNC_CLIENT = 0x395F2EC1
MARKER_KAD_NODE_ID = 0x04
KADEMLIA2_PUBLISH_KEY_REQ = 0x43
KADEMLIA2_PUBLISH_NOTES_REQ = 0x45
KADEMLIA_FIREWALLED_REQ = 0x50
KADEMLIA_FINDBUDDY_REQ = 0x51
KADEMLIA_CALLBACK_REQ = 0x52
KADEMLIA_FIREWALLED2_REQ = 0x53
# An unused TCP port, for the requests that queue a dial back to the sender.
DIAL_PORT = struct.pack('<H', 1)


def rc4(key, data):
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) & 0xff
        state[i], state[j] = state[j], state[i]
    i = j = 0
    out = bytearray()
    for byte in data:
        i = (i + 1) & 0xff
        j = (j + state[i]) & 0xff
        state[i], state[j] = state[j], state[i]
        out.append(byte ^ state[(state[i] + state[j]) & 0xff])
    return bytes(out)


def verify_key(ip):
    """CPrefs::GetUDPVerifyKey: the key the daemon hands out to this IP."""
    words = struct.unpack('<4I', hashlib.md5(socket.inet_aton(ip) + struct.pack('<I', UDP_KEY_SECRET)).digest())
    return (words[0] ^ words[1] ^ words[2] ^ words[3]) % 0xFFFFFFFE + 1


def encrypted(packet, kad_id, receiver_key):
    """As EncryptSendClient sends to a known Kad ID."""
    random_part = os.urandom(2)
    key = hashlib.md5(kad_id + random_part).digest()
    body = struct.pack('<IBII', MAGIC_UDP_SYNC_CLIENT, 0, receiver_key, 0x11111111) + packet
    return bytes([MARKER_KAD_NODE_ID]) + random_part + rc4(key, body)


def run(binary):
    with tempfile.TemporaryDirectory(prefix='kad-publish-key-') as directory:
        root = Path(directory)
        with running_kad_daemon(binary, root, console='stdout.log', VerboseDebug=1, VerboseDebugLogfile=1,
                                sections={'Debug': {'Cat_Client Kademlia UDP': 1},
                                          'Obfuscation': {'CryptoKadUDPKey': UDP_KEY_SECRET}}) as (proc, ec, udp_port, _):
            file_id = kad_id(ec, proc, root)

            keyed = lambda p: encrypted(p, file_id, verify_key('127.0.0.1'))

            def source_publish(tcp_port):
                # A different TCP port each: a source with the same IP and port would replace the last.
                tags = core_tags(tcp_port)
                return (KADEMLIA2_PUBLISH_SOURCE_REQ,
                        file_id + os.urandom(16) + bytes([len(tags)]) + b''.join(tags))

            for name, wrap, expected, (opcode, body) in (
                    ('Kad2 source publish', lambda p: p, 'not valid', source_publish(4001)),
                    ('Kad2 source publish', keyed, 'valid', source_publish(4002)),
                    ('Kad2 keyword publish', keyed, 'valid', (KADEMLIA2_PUBLISH_KEY_REQ, file_id + bytes(2))),
                    ('Kad2 notes publish', keyed, 'valid',
                     (KADEMLIA2_PUBLISH_NOTES_REQ, file_id + os.urandom(16) + bytes(1))),
                    ('Kad firewall check', keyed, 'valid', (KADEMLIA_FIREWALLED_REQ, DIAL_PORT)),
                    ('Kad firewall2 check', keyed, 'valid',
                     (KADEMLIA_FIREWALLED2_REQ, DIAL_PORT + os.urandom(16) + bytes(1))),
                    ('Kad find buddy request', keyed, 'valid',
                     (KADEMLIA_FINDBUDDY_REQ, os.urandom(32) + DIAL_PORT)),
                    ('Kad callback request', keyed, 'valid', (KADEMLIA_CALLBACK_REQ, os.urandom(34)))):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                    peer.bind(('127.0.0.1', 0))
                    peer.sendto(wrap(bytes([0xE4, opcode]) + body), ('127.0.0.1', udp_port))
                    line = f'{name} from 127.0.0.1:{peer.getsockname()[1]}: receiver key {expected}\n'
                    wait_for(proc, f'the log line {line!r}',
                             lambda: line in (root / 'logfile').read_text(errors='replace'), 10, interval=0.05)

            indexed = search(udp_port, file_id)
            assert len(indexed) == 2, f'{len(indexed)} of 2 publishes were indexed'
    print('each request logged its result, and both source publishes were indexed')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
