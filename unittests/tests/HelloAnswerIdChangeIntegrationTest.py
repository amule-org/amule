#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A hello answer that changes the ID of a client the daemon dialled must keep that client.

A server asks the daemon's LowID to call a peer back. The daemon makes a client for the peer and
dials it. Only the client list's ID map holds a reference to that client: it has no user address
and no valid user hash yet. The peer answers with another ID and the eMule marker with no other
bytes as its user hash, which many peers send and which counts as no hash, so no hash-list entry
is added. Moving the client to its new ID must not drop its only reference while the hello is
still being handled.

The daemon runs with glibc's malloc perturbation, so the failure shows in an ordinary build and
not only under a sanitizer. Without glibc or a sanitizer the test skips. A sanitizer runtime
named in AMULE_TEST_PRELOAD_RUNTIME comes before the shim in LD_PRELOAD.
"""
import os
from pathlib import Path
import platform
import socket
import struct
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import C, integer, retry_port_collisions, running_daemon, string
from DaemonConfig import daemon_env
from OfferFilesInteropPeer import receive, send
from SearchParamsRestoreIntegrationTest import configure

# FakePeerConnect sends the daemon's dials to this address to 127.0.0.1. A server callback
# request may not name a loopback address.
FAKE_PEER = '10.255.138.7'
OP_LOGINREQUEST = 0x01
OP_HELLO = 0x01
OP_IDCHANGE = 0x40
OP_CALLBACKREQUESTED = 0x35
OP_HELLOANSWER = 0x4c
# Not the ID the daemon made the client with.
ANSWERED_ID = 0x0a8aff0a
# The eMule marker with no other bytes: IsBadUserHash() counts it as no hash.
MARKER_ONLY_HASH = bytes.fromhex('00000000000E00000000000000006F00')
PEER_NAME = b'peer'
# Without it, an ordinary build can pass by chance.
MALLOC_PERTURB = 'glibc.malloc.perturb=165'


def listening():
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    return listener


def hello_answer(user_hash, user_id):
    tags = [struct.pack('<BBH', 0x82, 0x01, len(PEER_NAME)) + PEER_NAME,  # CT_NAME
            struct.pack('<BBI', 0x83, 0x11, 0x3c)]  # CT_VERSION
    return (user_hash + struct.pack('<IHI', user_id, 4662, len(tags)) + b''.join(tags)
            + struct.pack('<IH', 0, 0))  # no server


def low_id_server(server, logged_in):
    """Grants a LowID and keeps the server connection open."""
    def serve():
        try:
            peer, _ = server.accept()
            while receive(peer)[0] != OP_LOGINREQUEST:
                pass
            send(peer, OP_IDCHANGE, struct.pack('<II', 1234, 0))
            logged_in['socket'] = peer
            logged_in['event'].set()
            while peer.recv(4096):
                pass
        except (EOFError, OSError):
            pass

    threading.Thread(target=serve, daemon=True).start()


def listed_names(ec):
    """The name of each client in the daemon's client list, as EC reports it."""
    opcode, tags = ec.call_list(C['EC_OP_GET_UPDATE'], [
        integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_INC_UPDATE'])])
    assert opcode == C['EC_OP_SHARED_FILES'], opcode
    names = []
    for name, _, children in tags:
        if name == C['EC_TAG_CLIENT']:
            for _, _, fields in children:
                values = {key: data for key, data, _ in fields}
                names.append(values.get(C['EC_TAG_CLIENT_NAME'], b'').rstrip(b'\0'))
    return names


def run(binary, shim):
    with tempfile.TemporaryDirectory(prefix='amule-hello-id-change-') as directory:
        root = Path(directory)
        ports = configure(root)
        preload = ':'.join(filter(None, [os.environ.get('AMULE_TEST_PRELOAD_RUNTIME'), shim]))
        tunables = ':'.join(filter(None, [os.environ.get('GLIBC_TUNABLES'), MALLOC_PERTURB]))
        env = daemon_env(root, LD_PRELOAD=preload, AMULE_TEST_FAKE_PEER=FAKE_PEER,
                         GLIBC_TUNABLES=tunables)
        with running_daemon(binary, root, ports, env=env) as (proc, ec), \
                listening() as server, listening() as callee:
            logged_in = {'event': threading.Event()}
            low_id_server(server, logged_in)
            assert ec.call(C['EC_OP_SERVER_ADD'], [
                string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{server.getsockname()[1]}')
            ])[0] == C['EC_OP_NOOP']
            assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
            assert logged_in['event'].wait(60), 'the daemon never logged in to the server'

            send(logged_in['socket'], OP_CALLBACKREQUESTED,
                 socket.inet_aton(FAKE_PEER) + struct.pack('<H', callee.getsockname()[1]))
            callee.settimeout(10)
            peer, _ = callee.accept()
            with peer:
                peer.settimeout(10)
                while receive(peer)[0] != OP_HELLO:
                    pass
                send(peer, OP_HELLOANSWER, hello_answer(MARKER_ONLY_HASH, ANSWERED_ID))
                time.sleep(2)
                assert proc.poll() is None, \
                    f'amuled is no longer running ({proc.returncode})'
                # Listed under its new ID while the peer is still connected: on disconnect the
                # daemon may drop a client it has no other use for.
                names = listed_names(ec)
                assert PEER_NAME in names, f'the dialled client is no longer listed: {names}'
        print('PASS: a dialled client survives a hello answer with another ID and a marker-only '
              'hash')


if __name__ == '__main__':
    # Only glibc honours the perturbation: elsewhere, without a sanitizer, the test would pass
    # without the fix.
    if platform.libc_ver()[0] != 'glibc' and not os.environ.get('AMULE_TEST_PRELOAD_RUNTIME'):
        print('SKIP: no glibc malloc perturbation and no sanitizer runtime')
        sys.exit(77)
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve()),
                                      str(Path(sys.argv[2]).resolve())))
