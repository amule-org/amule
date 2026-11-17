#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A ban for aggressive upload requests outlives the banned client's place in the queue.

Two peers take both upload slots and a third waits. The waiting peer asks for its upload too
often and is banned. When a slot frees, the banned peer must not get it. When the banned peer
disconnects, its client is deleted and leaves the queue, and its address must stay banned.
"""
import argparse
import os
from pathlib import Path
import socket
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon, wait_for
from DaemonConfig import write_amule_conf
from DownloadDisplayRatesIntegrationTest import hello
from ShareWatcherMoveIntegrationTest import wait_shared
from UploadRefillIntegrationTest import packet, send
from UploadSlotScopeIntegrationTest import (OP_ACCEPTUPLOADREQ, OP_CANCELTRANSFER, OP_HELLOANSWER,
                                            OP_STARTUPLOADREQ, Peer)

# A request less than 590 s after the previous one adds 3 to the score; 10 bans. The first only
# starts the clock, so the fifth bans.
BANNING_REQUESTS = 5


def queue_and_bans(ec):
    op, tags = ec.call(C['EC_OP_STAT_REQ'])
    assert op == C['EC_OP_STATS'], op
    return tags[C['EC_TAG_STATS_UL_QUEUE_LEN']][0], tags[C['EC_TAG_STATS_BANNED_COUNT']][0]


def refused(address, port):
    """True if the daemon closes a new connection from address, False if it answers the hello."""
    with socket.socket() as sock:
        sock.bind((address, 0))
        sock.connect((address, port))
        sock.settimeout(10)
        try:
            send(sock, 0x01, hello(b'ban-probe', address, sock.getsockname()[1]))
            while packet(sock)[1] != OP_HELLOANSWER:
                pass
        except (ConnectionError, EOFError):
            return True
    return False


def run(binary, address):
    with tempfile.TemporaryDirectory(prefix='amule-client-ban-lifetime-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        (root / 'Incoming' / 'shared.bin').write_bytes(os.urandom(1 << 20))
        ec_port, peer_port = free_ports(2, peer_index=1)
        # Below 10 kB/s the daemon offers its minimum of two upload slots.
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0, MaxUpload=5)
        peers = []
        with running_daemon(binary, root, (ec_port, peer_port)) as (proc, ec):
            try:
                file_hash = wait_shared(proc, ec, ['shared.bin'], 30)['shared.bin']

                def open_peer(name):
                    peer = Peer(address, address, peer_port, name, file_hash)
                    peers.append(peer)
                    return peer

                for name in ('ban-holder-1', 'ban-holder-2'):
                    assert open_peer(name).wait_for(OP_ACCEPTUPLOADREQ, 10), f'{name} got no slot'
                aggressive = open_peer('ban-aggressive')
                wait_for(proc, 'the third peer to be queued', lambda: queue_and_bans(ec) == (1, 0), 10)
                for _ in range(BANNING_REQUESTS - 1):
                    send(aggressive.sock, OP_STARTUPLOADREQ, file_hash)
                wait_for(proc, 'the ban', lambda: queue_and_bans(ec) == (1, 1), 10)
                assert refused(address, peer_port), 'a banned address was accepted'

                send(peers[0].sock, OP_CANCELTRANSFER)
                assert not aggressive.wait_for(OP_ACCEPTUPLOADREQ, 4), 'the banned peer got the free slot'

                aggressive.close()
                wait_for(proc, 'the banned peer to leave the queue', lambda: queue_and_bans(ec)[0] == 0, 10)
                assert queue_and_bans(ec)[1] == 1, 'the ban ended when the client was deleted'
                assert refused(address, peer_port), 'the address was accepted after its client was deleted'
            finally:
                for peer in peers:
                    peer.close()
    print('the banned peer got no free slot, and its address stayed banned after its client was deleted')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--address', default='127.0.0.1', help='local address for the peers')
    args = parser.parse_args()
    retry_port_collisions(run, str(Path(args.binary).resolve()), args.address)
