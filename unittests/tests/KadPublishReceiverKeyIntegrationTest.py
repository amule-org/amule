#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Kad source publish logs whether its receiver key proves the sender receives at its IP.

A plain and a keyed publish each log their own result, and both are still indexed. Two is all the
per-IP limit lets through in a minute.
"""
import hashlib
import os
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from KadSourceTagsECTest import KADEMLIA2_PUBLISH_SOURCE_REQ, core_tags, search, wait_ready

UDP_KEY_SECRET = 0x5eed1234
MAGIC_UDP_SYNC_CLIENT = 0x395F2EC1
MARKER_KAD_NODE_ID = 0x04


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
        port, udp_port, peer_port = free_ports(3, udp_indices=(1,), peer_index=2)
        write_amule_conf(root, port, Port=peer_port, UDPPort=udp_port, ConnectToKad=1, Autoconnect=0,
                         VerboseDebug=1, VerboseDebugLogfile=1,
                         sections={'Debug': {'Cat_Client Kademlia UDP': 1},
                                   'Obfuscation': {'CryptoKadUDPKey': UDP_KEY_SECRET}})
        with running_daemon(binary, root, (port, peer_port, udp_port), console='stdout.log') as (proc, ec):
            # Stopping Kad saves the index, whose header names our Kad ID.
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            wait_ready(ec, proc)
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
            file_id = (root / 'key_index.dat').read_bytes()[8:24]
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            wait_ready(ec, proc)

            # A different TCP port each: a source with the same IP and port would replace the last.
            for tcp_port, wrap, expected in (
                    (4001, lambda p: p, 'not valid'),
                    (4002, lambda p: encrypted(p, file_id, verify_key('127.0.0.1')), 'valid')):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
                    peer.bind(('127.0.0.1', 0))
                    tags = core_tags(tcp_port)
                    packet = (bytes([0xE4, KADEMLIA2_PUBLISH_SOURCE_REQ]) + file_id + os.urandom(16)
                              + bytes([len(tags)]) + b''.join(tags))
                    peer.sendto(wrap(packet), ('127.0.0.1', udp_port))
                    line = (f'Kad2 source publish from 127.0.0.1:{peer.getsockname()[1]}: '
                            f'receiver key {expected}\n')
                    deadline = time.monotonic() + 10
                    while line not in (root / 'logfile').read_text(errors='replace'):
                        assert time.monotonic() < deadline, f'not logged: {line}'
                        time.sleep(0.05)

            indexed = search(udp_port, file_id)
            assert len(indexed) == 2, f'{len(indexed)} of 2 publishes were indexed'
    print('a plain and a keyed publish each logged their result and were indexed')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
