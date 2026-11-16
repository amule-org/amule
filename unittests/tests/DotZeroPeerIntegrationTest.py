#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A HighID peer at an IPv4 ending in .0 must be dialled, not treated as a LowID.

The ed2k ID of such an address is below the LowID limit. Each case gives the daemon a peer
only by its address and expects a TCP connection to that address.
"""
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions, start_daemon,
                                      stop_daemon, string, tag)
from DaemonConfig import daemon_env, write_amule_conf

# FakePeerConnect sends the daemon's dials to this address to 127.0.0.1. It is not loopback,
# which a server callback request may not name.
FAKE_PEER = '10.255.138.0'


def gui_id(address, port):
    return (struct.unpack('<I', socket.inet_aton(address))[0] << 16) + port


def ed2k_packet(opcode, body):
    return struct.pack('<BIB', 0xe3, len(body) + 1, opcode) + body


def dialled(listener):
    listener.settimeout(10)
    try:
        peer, _ = listener.accept()
    except socket.timeout:
        return False
    peer.close()
    return True


def listening(address):
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((address, 0))
    listener.listen(1)
    return listener


def low_id_server(callback_port):
    """Grants a LowID, then asks the daemon to call FAKE_PEER back."""
    server = listening('127.0.0.1')

    def serve():
        try:
            peer, _ = server.accept()
            while True:
                _, length = struct.unpack('<BI', exact(peer, 5))
                if exact(peer, length)[0] == 0x01:  # OP_LOGINREQUEST
                    break
            peer.sendall(ed2k_packet(0x40, struct.pack('<II', 1234, 0))  # OP_IDCHANGE, a LowID
                         + ed2k_packet(0x35, socket.inet_aton(FAKE_PEER)  # OP_CALLBACKREQUESTED
                                       + struct.pack('<H', callback_port)))
            while peer.recv(4096):
                pass
        except (EOFError, OSError):
            pass

    threading.Thread(target=serve, daemon=True).start()
    return server


def run(binary, shim):
    with tempfile.TemporaryDirectory(prefix='amule-dot-zero-peer-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToKad=1,
                         ConnectToED2K=1, FilterLanIPs=0)
        preload = ':'.join(filter(None, [os.environ.get('AMULE_TEST_PRELOAD_RUNTIME'), shim]))
        env = daemon_env(root, LD_PRELOAD=preload, AMULE_TEST_FAKE_PEER=FAKE_PEER)
        proc, ec = start_daemon(binary, root, ports, env=env)
        undialled = []
        try:
            # A direct callback request, accepted while Kad is firewalled.
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, \
                    listening('127.1.2.0') as listener:
                udp.bind(('127.1.2.0', 0))
                udp.sendto(b'\xc5\x95' + struct.pack('<H', listener.getsockname()[1])
                           + bytes(range(16)) + b'\0', ('127.0.0.1', ports[2]))
                if not dialled(listener):
                    undialled.append('direct UDP callback')

            # A chat to a peer known by hash, with its address as the route.
            with listening('127.1.3.0') as listener:
                port = listener.getsockname()[1]
                assert ec.call(C['EC_OP_CHAT_SEND'], [
                    tag(C['EC_TAG_CHAT_PEER_HASH'], bytes(range(16, 32)), 9),
                    tag(C['EC_TAG_CHAT_CLIENT_ID'], struct.pack('!Q', gui_id('127.1.3.0', port)), 5),
                    string(C['EC_TAG_CHAT'], 'hello')])[0] == C['EC_OP_NOOP']
                if not dialled(listener):
                    undialled.append('chat by hash')

            # A chat to a bare address, as a legacy remote GUI sends it.
            with listening('127.1.4.0') as listener:
                port = listener.getsockname()[1]
                assert ec.call(C['EC_OP_CHAT_SEND'], [
                    tag(C['EC_TAG_CHAT_CLIENT_ID'], struct.pack('!Q', gui_id('127.1.4.0', port)), 5),
                    string(C['EC_TAG_CHAT'], 'hello')])[0] == C['EC_OP_NOOP']
                if not dialled(listener):
                    undialled.append('chat by address')

            # A server asks our LowID to call a HighID back.
            with listening('127.0.0.1') as listener:
                server = low_id_server(listener.getsockname()[1])
                assert ec.call(C['EC_OP_SERVER_ADD'], [
                    string(C['EC_TAG_SERVER_ADDRESS'], f'localhost:{server.getsockname()[1]}')
                ])[0] == C['EC_OP_NOOP']
                assert ec.call(C['EC_OP_SERVER_CONNECT'])[0] == C['EC_OP_NOOP']
                if not dialled(listener):
                    undialled.append('server callback request')
                server.close()
            assert not undialled, f'.0 peer never dialled after: {", ".join(undialled)}'
            print('PASS: direct callback, chat by hash, chat by address and server callback all '
                  'dial a .0 peer')
        except BaseException:
            print((root / 'logfile').read_text(errors='replace')[-6000:], file=sys.stderr)
            raise
        finally:
            ec.sock.close()
            stop_daemon(proc)


if __name__ == '__main__':
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve()),
                                      str(Path(sys.argv[2]).resolve())))
