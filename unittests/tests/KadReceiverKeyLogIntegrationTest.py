#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Kad requests log whether their receiver key proves the sender receives at its IP.

A plain and a keyed source publish each log their own result, and both are still indexed. Two is all
the per-IP limit lets through in a minute. The other requests share the logging, so one keyed packet
each is enough, and the callback limit allows only one.
"""
import contextlib
import hashlib
import os
import socket
import struct
import sys
import tempfile
from pathlib import Path

from AllSearchIntegrationTest import retry_port_collisions, wait_for
from KadSourceTagsECTest import (FILE_SIZE, KADEMLIA2_PUBLISH_SOURCE_REQ, TAG_ENCRYPTION, TAG_FILESIZE,
                                 TAG_SOURCEPORT, TAG_SOURCETYPE, core_tags, kad_id, running_kad_daemon,
                                 search, tag)

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


@contextlib.contextmanager
def logging_daemon(binary, prefix):
    """A daemon whose Kad UDP log reaches the logfile, and whose UDP key secret we know."""
    with tempfile.TemporaryDirectory(prefix=prefix) as directory:
        root = Path(directory)
        with running_kad_daemon(binary, root, console='stdout.log', VerboseDebug=1, VerboseDebugLogfile=1,
                                sections={'Debug': {'Cat_Client Kademlia UDP': 1},
                                          'Obfuscation': {'CryptoKadUDPKey': UDP_KEY_SECRET}}) as (proc, ec, udp_port, _):
            yield root, proc, ec, udp_port


def send_expecting(root, proc, udp_port, packet, expected):
    """Send one packet from a fresh port and wait for its log line; {port} is that port."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind(('127.0.0.1', 0))
        peer.sendto(packet, ('127.0.0.1', udp_port))
        line = expected.format(port=peer.getsockname()[1]) + '\n'
        wait_for(proc, f'the log line {line!r}',
                 lambda: line in (root / 'logfile').read_text(errors='replace'), 10, interval=0.05)


def keyed_for(packet, file_id):
    """Obfuscated to the daemon's Kad ID and carrying the key it hands out to 127.0.0.1."""
    return encrypted(packet, file_id, verify_key('127.0.0.1'))


def source_publish(file_id, tags):
    """A publish at the daemon's own Kad ID, so it passes the store tolerance.

    Callers give each record a different TCP port: one with the same IP and port replaces the last.
    """
    return (KADEMLIA2_PUBLISH_SOURCE_REQ,
            file_id + os.urandom(16) + bytes([len(tags)]) + b''.join(tags))


def type7_tags(tcp_port, pointer_repeats=False):
    """A "routes in tags" record: one route, no buddy tags, size always present.

    With pointer_repeats, a second "bpv" follows the first and is a long string rather than the
    version byte. Only the first integer may be stored, so the repeat must not come back.
    """
    tags = [tag(TAG_SOURCETYPE, 0x09, 7), tag(TAG_SOURCEPORT, 0x08, tcp_port),
            tag(TAG_FILESIZE, 0x03, FILE_SIZE), tag(TAG_ENCRYPTION, 0x09, 3),
            tag(b'ip6', 0x02, b'20010db8' * 4)]
    if pointer_repeats:
        tags += [tag(b'bpv', 0x09, 1), tag(b'bpv', 0x02, b'r' * 400)]
    return tags


def pointer_record(file_id, tcp_port):
    """A publish whose key is the bitwise NOT of its own source ID, which is what marks a pointer.

    The key stays the daemon's own Kad ID, so the publish still passes the store tolerance.
    """
    tags = [tag(TAG_SOURCETYPE, 0x09, 3), tag(TAG_SOURCEPORT, 0x08, tcp_port),
            tag(TAG_FILESIZE, 0x03, FILE_SIZE)]
    source_id = bytes(~b & 0xFF for b in file_id)
    return (KADEMLIA2_PUBLISH_SOURCE_REQ,
            file_id + source_id + bytes([len(tags)]) + b''.join(tags))


def pointer_tagged_record(file_id, tcp_port):
    """A classic type 3 record carrying the pointer marker, which storers keep only with a key."""
    tags = [tag(TAG_SOURCETYPE, 0x09, 3), tag(TAG_SOURCEPORT, 0x08, tcp_port),
            tag(TAG_FILESIZE, 0x03, FILE_SIZE), tag(b'bpv', 0x09, 1)]
    return source_publish(file_id, tags)


def drop_unkeyed_pointer_records(binary):
    """Neither the pointer marker nor a pointer key survives a publish that proves no sender.

    Its own daemon again: the publish bucket is 2 a minute per IP, and these are two more. The log
    line is the whole assertion: it is written where the handler returns, before anything is
    indexed and before a response goes back, and a search with nothing stored simply never answers.
    """
    with logging_daemon(binary, 'kad-pointer-drop-') as (root, proc, ec, udp_port):
        file_id = kad_id(ec, proc, root)
        for (opcode, body), expected in (
                (pointer_tagged_record(file_id, 4201),
                 'Dropped Kad2 source publish of type 3 with a pointer tag from 127.0.0.1:{port}: '
                 'the receiver key did not prove the sender'),
                (pointer_record(file_id, 4202),
                 'Dropped Kad2 source publish of type 3 at a pointer key from 127.0.0.1:{port}: '
                 'the receiver key did not prove the sender')):
            send_expecting(root, proc, udp_port, bytes([0xE4, opcode]) + body, expected)


def drop_unkeyed_requests(binary):
    """What must prove its sender is dropped when the receiver key does not, and nothing else is.

    Its own daemon for two reasons: the per-IP tracker counts both firewall opcodes in one bucket
    of 2 a minute and run() spends both, and the publish bucket is 2 a minute as well.
    """
    with logging_daemon(binary, 'kad-unkeyed-drop-') as (root, proc, ec, udp_port):
        file_id = kad_id(ec, proc, root)
        keyed = lambda p: keyed_for(p, file_id)

        # The keyed type 7 publish is the control: same record, and it is indexed.
        for wrap, (opcode, body), expected in (
                (keyed, source_publish(file_id, type7_tags(4101, pointer_repeats=True)),
                 'Kad2 source publish from 127.0.0.1:{port}: receiver key valid'),
                (lambda p: p, source_publish(file_id, type7_tags(4102)),
                 'Dropped Kad2 source publish of type 7 from 127.0.0.1:{port}: '
                 'the receiver key did not prove the sender'),
                (lambda p: p, (KADEMLIA_FIREWALLED2_REQ, DIAL_PORT + os.urandom(16) + bytes(1)),
                 'Dropped Kad firewall2 check from 127.0.0.1:{port}: '
                 'the receiver key did not prove the sender')):
            send_expecting(root, proc, udp_port, wrap(bytes([0xE4, opcode]) + body), expected)

        indexed = search(udp_port, file_id)
        assert len(indexed) == 1, f'{len(indexed)} of 1 type 7 publishes were indexed'
        (stored,) = indexed.values()
        # The version byte, not the 400-byte repeat that followed it.
        assert stored.get(b'bpv') == 1, ('a repeated bpv was stored', stored.get(b'bpv'))


def run(binary):
    with logging_daemon(binary, 'kad-publish-key-') as (root, proc, ec, udp_port):
        file_id = kad_id(ec, proc, root)
        keyed = lambda p: keyed_for(p, file_id)

        for name, wrap, expected, (opcode, body) in (
                ('Kad2 source publish', lambda p: p, 'not valid',
                 source_publish(file_id, core_tags(4001))),
                ('Kad2 source publish', keyed, 'valid',
                 source_publish(file_id, core_tags(4002))),
                ('Kad2 keyword publish', keyed, 'valid', (KADEMLIA2_PUBLISH_KEY_REQ, file_id + bytes(2))),
                ('Kad2 notes publish', keyed, 'valid',
                 (KADEMLIA2_PUBLISH_NOTES_REQ, file_id + os.urandom(16) + bytes(1))),
                ('Kad firewall check', keyed, 'valid', (KADEMLIA_FIREWALLED_REQ, DIAL_PORT)),
                ('Kad firewall2 check', keyed, 'valid',
                 (KADEMLIA_FIREWALLED2_REQ, DIAL_PORT + os.urandom(16) + bytes(1))),
                ('Kad find buddy request', keyed, 'valid',
                 (KADEMLIA_FINDBUDDY_REQ, os.urandom(32) + DIAL_PORT)),
                ('Kad callback request', keyed, 'valid', (KADEMLIA_CALLBACK_REQ, os.urandom(34)))):
            send_expecting(root, proc, udp_port, wrap(bytes([0xE4, opcode]) + body),
                           f'{name} from 127.0.0.1:{{port}}: receiver key {expected}')

        indexed = search(udp_port, file_id)
        assert len(indexed) == 2, f'{len(indexed)} of 2 publishes were indexed'
    drop_unkeyed_requests(binary)
    drop_unkeyed_pointer_records(binary)
    print('each request logged its result, the unkeyed firewall2 check and the type 7, pointer-tag '
          'and pointer-key publishes were dropped, and the keyed publishes were indexed')


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1])
