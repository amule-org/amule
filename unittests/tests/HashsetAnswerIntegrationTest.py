#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""OP_HASHSETREQUEST is answered for a file of any size.

A file under one part has no part hashes, and the answer says so with a count of 0: a peer that
gets no answer waits for it until the connection times out. A file of exactly one part has two
part hashes, the second being the MD4 of nothing. A download that should have part hashes but
has none yet gets no answer, as a count of 0 would be a bad hashset to the peer.
"""
from pathlib import Path
import socket
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from CancelCompletedIntegrationTest import md4
from DownloadDisplayRatesIntegrationTest import hello
from ED2KLinkIntegrationTest import add_link
from ShareWatcherMoveIntegrationTest import wait_shared
from UploadRefillIntegrationTest import send, wait_packet
from UploadSlotScopeIntegrationTest import OP_HELLOANSWER

PARTSIZE = 9728000
EMPTY_MD4 = bytes.fromhex('31D6CFE0D16AE931B73C59D7E0C089C0')
OP_HASHSETREQUEST = 0x51
OP_HASHSETANSWER = 0x52


def greet(sock):
    send(sock, 0x01, hello(b'hashset-peer', '127.0.0.1', sock.getsockname()[1]))
    assert wait_packet(sock, OP_HELLOANSWER, 10) is not None, 'no answer to the hello'


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-hashset-answer-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Incoming' / 'small.bin').write_bytes(b'x' * (1 << 20))
        (root / 'Incoming' / 'onepart.bin').write_bytes(b'x' * PARTSIZE)
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0)
        with running_daemon(binary, root, (ec_port, peer_port)) as (proc, ec):
            files = wait_shared(proc, ec, ['small.bin', 'onepart.bin'], 60)
            with socket.create_connection(('127.0.0.1', peer_port)) as sock:
                greet(sock)
                send(sock, OP_HASHSETREQUEST, files['small.bin'])
                answer = wait_packet(sock, OP_HASHSETANSWER, 10)
                assert answer is not None, 'no answer for a file under one part'
                assert answer == files['small.bin'] + b'\0\0', answer.hex()

                send(sock, OP_HASHSETREQUEST, files['onepart.bin'])
                answer = wait_packet(sock, OP_HASHSETANSWER, 10)
                assert answer is not None, 'no answer for a file of exactly one part'
                assert answer[:18] == files['onepart.bin'] + b'\2\0', answer.hex()
                assert len(answer) == 18 + 2 * 16, answer.hex()
                assert answer[34:] == EMPTY_MD4, answer.hex()

                # A queued download with no part hashes yet gets no answer, so the next answer is
                # for the one under one part, which has none to wait for.
                queued, queued_small = bytes([1] * 16), bytes([2] * 16)
                for name, size, file_hash in (('queued.bin', 2 * PARTSIZE, queued),
                                              ('queued-small.bin', 1 << 20, queued_small)):
                    link = f'ed2k://|file|{name}|{size}|{file_hash.hex()}|/'
                    assert add_link(ec, link) == C['EC_OP_NOOP'], name
                send(sock, OP_HASHSETREQUEST, queued)
                send(sock, OP_HASHSETREQUEST, queued_small)
                answer = wait_packet(sock, OP_HASHSETANSWER, 10)
                assert answer is not None, 'no answer for a queued download under one part'
                assert answer == queued_small + b'\0\0', answer.hex()

                # The link parser hands its buffer to CPartFile only after all fields
                # validate. Read the loaded hashes back through the real peer protocol.
                for index, hashes in enumerate((bytes(range(32)), bytes(range(32, 64)))):
                    file_hash = md4(hashes)
                    parts = ':'.join(hashes[i:i + 16].hex() for i in (0, 16))
                    # Repeated p= fields keep the last set.
                    fields = (f'p={EMPTY_MD4.hex()}|' if index else '') + f'p={parts}|'
                    link = (f'ed2k://|file|linked-{index}.bin|{PARTSIZE + 1}|'
                            f'{file_hash.hex()}|{fields}/')
                    assert add_link(ec, link) == C['EC_OP_NOOP'], link
                    send(sock, OP_HASHSETREQUEST, file_hash)
                    answer = wait_packet(sock, OP_HASHSETANSWER, 10)
                    assert answer == file_hash + b'\2\0' + hashes, (
                        'link part hashes were not retained', answer)
    print('a file or download under one part answered with no part hashes; one of exactly one '
          'part with two; a download still missing its hashes got no answer')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
