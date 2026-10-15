#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publish Kad sources with other clients' tags to an isolated daemon and read them back."""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path
from AllSearchIntegrationTest import C, connect_daemon, free_ports, retry_port_collisions
from DaemonConfig import write_amule_conf

KADEMLIA2_SEARCH_SOURCE_REQ = 0x34
KADEMLIA2_SEARCH_RES = 0x3B
KADEMLIA2_PUBLISH_SOURCE_REQ = 0x44
KADEMLIA2_PUBLISH_RES = 0x4B
TAG_SOURCETYPE, TAG_SOURCEPORT, TAG_FILESIZE = b'\xff', b'\xfd', b'\x02'
TAG_SERVERIP, TAG_SERVERPORT, TAG_BUDDYHASH, TAG_ENCRYPTION = b'\xfb', b'\xfa', b'\xf8', b'\xf3'
FILE_SIZE = 9728000


def tag(name, kind, value):
    head = bytes([kind]) + struct.pack('<H', len(name)) + name
    if kind == 0x02:
        return head + struct.pack('<H', len(value)) + value
    if kind == 0x0A:
        return head + bytes([len(value)]) + value
    return head + struct.pack({0x03: '<I', 0x08: '<H', 0x09: '<B'}[kind], value)


def parse_tags(data, offset, count):
    tags = {}
    for _ in range(count):
        kind = data[offset]
        length = struct.unpack_from('<H', data, offset + 1)[0]
        name = data[offset + 3:offset + 3 + length]
        offset += 3 + length
        if kind == 0x02:
            size = struct.unpack_from('<H', data, offset)[0]
            tags[name] = data[offset + 2:offset + 2 + size]
            offset += 2 + size
        elif kind == 0x0A:
            tags[name] = data[offset + 1:offset + 1 + data[offset]]
            offset += 1 + data[offset]
        else:
            size = {0x03: 4, 0x08: 2, 0x09: 1, 0x0B: 8}[kind]
            tags[name] = int.from_bytes(data[offset:offset + size], 'little')
            offset += size
    return tags, offset


def core_tags(tcp_port):
    # A firewalled source behind a buddy with IPv6, as eMuleAI and emule-qt publish it.
    return [tag(TAG_SOURCETYPE, 0x09, 3), tag(TAG_SERVERIP, 0x03, 0x0100007f),
            tag(TAG_SERVERPORT, 0x08, 4672), tag(TAG_BUDDYHASH, 0x02, b'ab' * 16),
            tag(TAG_SOURCEPORT, 0x08, tcp_port), tag(TAG_FILESIZE, 0x03, FILE_SIZE),
            tag(TAG_ENCRYPTION, 0x09, 3), tag(b'ip6', 0x02, b'20010db8' * 4),
            tag(b'bi6', 0x02, b'20010db9' * 4)]


def http_cache_tags():
    # emule-qt's largest addition: three chunks of HTTP cache hints, about 1.15 KB.
    tags = []
    for i in range(3):
        n = str(i).encode()
        tags += [tag(b'hcp' + n, 0x03, i), tag(b'hcu' + n, 0x02, b'u' * 256),
                 tag(b'hck' + n, 0x0A, bytes(48)), tag(b'hcs' + n, 0x0A, bytes(32)),
                 tag(b'hce' + n, 0x03, 2000000000)]
    return tags


def publish(udp_port, file_id, source_id, tags):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind(('127.0.0.1', 0))
        peer.settimeout(10)
        peer.sendto(bytes([0xE4, KADEMLIA2_PUBLISH_SOURCE_REQ]) + file_id + source_id
                    + bytes([len(tags)]) + b''.join(tags), ('127.0.0.1', udp_port))
        response, _ = peer.recvfrom(65535)
        assert response[:2] == bytes([0xE4, KADEMLIA2_PUBLISH_RES]), ('publish', response)


def search(udp_port, file_id):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind(('127.0.0.1', 0))
        peer.settimeout(10)
        peer.sendto(bytes([0xE4, KADEMLIA2_SEARCH_SOURCE_REQ]) + file_id
                    + struct.pack('<HQ', 0, FILE_SIZE), ('127.0.0.1', udp_port))
        response, _ = peer.recvfrom(65535)
    if response[0] == 0xE5:
        # OP_KADEMLIAPACKEDPROT: the payload after the opcode is zlib-compressed.
        response = bytes([0xE4, response[1]]) + zlib.decompress(response[2:])
    assert response[:2] == bytes([0xE4, KADEMLIA2_SEARCH_RES]), ('search', response)
    count = struct.unpack_from('<H', response, 34)[0]
    offset, results = 36, {}
    for _ in range(count):
        answer = response[offset:offset + 16]
        tags, offset = parse_tags(response, offset + 17, response[offset + 16])
        results[answer] = tags
    return results


def wait_ready(ec, proc):
    deadline = time.monotonic() + 120
    while C['EC_TAG_STATS_KAD_INDEXED_SOURCES'] not in ec.call(C['EC_OP_STAT_REQ'])[1]:
        assert proc.poll() is None, 'amuled exited while loading'
        assert time.monotonic() < deadline, 'index never became ready'
        time.sleep(0.05)


def run(binary):
    with tempfile.TemporaryDirectory(prefix='kad-source-tags-') as directory:
        root = Path(directory)
        port, udp_port, peer_port = free_ports(3, udp_indices=(1,), peer_index=2)
        write_amule_conf(root, port, Port=peer_port, UDPPort=udp_port, ConnectToKad=1,
                         Autoconnect=0)
        with (root / 'stdout.log').open('w') as log:
            env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
            proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
            try:
                ec = connect_daemon(proc, port, root, peer_port, udp_port)
                # Stopping Kad saves the index, whose header names our Kad ID: a publish
                # aimed at it is within tolerance.
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                wait_ready(ec, proc)
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                file_id = (root / 'key_index.dat').read_bytes()[8:24]
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                wait_ready(ec, proc)

                cache_source, big_source = b'\x11' * 16, b'\x22' * 16
                publish(udp_port, file_id, cache_source, core_tags(4001) + http_cache_tags())
                oversized = [tag(b'xa', 0x02, b'a' * 1500), tag(b'xb', 0x02, b'b' * 1500),
                             tag(b'xc', 0x09, 1), tag(TAG_ENCRYPTION, 0x09, 1)]
                # Same core tags, but an over-long buddy hash and the encryption tag last.
                big_core = [tag(TAG_BUDDYHASH, 0x02, b'c' * 100) if t.startswith(b'\x02\x01\x00\xf8')
                            else t for t in core_tags(4002) if not t.startswith(b'\x09\x01\x00\xf3')]
                publish(udp_port, file_id, big_source, big_core + oversized)

                results = search(udp_port, file_id)
                cached = results[cache_source]
                for name in (b'ip6', b'bi6', TAG_BUDDYHASH, TAG_SERVERIP, TAG_ENCRYPTION):
                    assert name in cached, ('known tag dropped', name)
                for i in range(3):
                    for prefix in (b'hcp', b'hcu', b'hck', b'hcs', b'hce'):
                        assert prefix + str(i).encode() in cached, ('cache tag dropped', prefix, i)
                assert cached[b'hcu2'] == b'u' * 256

                big = results[big_source]
                assert big[TAG_SOURCEPORT] == 4002, big
                assert b'ip6' in big and b'bi6' in big, big
                assert TAG_BUDDYHASH not in big, 'over-long buddy hash kept'
                assert big[b'xa'] == b'a' * 1500, 'first extra tag within budget dropped'
                assert b'xb' not in big and b'xc' not in big, 'extra tags past the budget kept'
                assert TAG_ENCRYPTION in big, 'known tag after the budget dropped'
                ec.sock.close()
                print('PASS: emule-qt cache tags, eMuleAI buddy and IPv6 tags kept; '
                      'extra tags past the per-source budget dropped, the source kept')
            except BaseException:
                log.flush()
                print((root / 'stdout.log').read_text(), file=sys.stderr)
                if (root / 'logfile').exists():
                    print((root / 'logfile').read_text()[-8000:], file=sys.stderr)
                raise
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
