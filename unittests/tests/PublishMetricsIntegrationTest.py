#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe real source publication, queued packets and duplicate replies on a private LAN peer."""
import ipaddress
import re
import socket
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path
from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf


def local_peer():
    try:
        addresses = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM)
    except OSError:
        addresses = []
    # Hostnames commonly resolve only to loopback on Linux desktops. Query owned
    # interfaces rather than probing a public destination or changing networking.
    if sys.platform.startswith('linux'):
        import fcntl
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            for _, name in socket.if_nameindex():
                try:
                    result = fcntl.ioctl(probe.fileno(), 0x8915, struct.pack('256s', name.encode()[:15]))
                    addresses.append((socket.AF_INET, socket.SOCK_DGRAM, 0, '',
                                      (socket.inet_ntoa(result[20:24]), 0)))
                except OSError:
                    pass
    for address in addresses:
        ip = address[4][0]
        if not ipaddress.ip_address(ip).is_private or ipaddress.ip_address(ip).is_loopback:
            continue
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            peer.bind((ip, 0))
            peer.settimeout(0.1)
            return peer
        except OSError:
            peer.close()
    return None


def fields(line):
    return {key: int(value) for key, value in re.findall(r'\b(\w+)=(\d+)(?=\s|$)', line)}


def run(binary):
    peer = local_peer()
    if peer is None:
        print('No owned private non-loopback IPv4 address for isolated Kad peer')
        return 77
    with peer, \
            tempfile.TemporaryDirectory(prefix='amule-publish-metrics-') as directory:
        root = Path(directory)
        incoming = root / 'Incoming'
        incoming.mkdir()
        (incoming / 'publication.bin').write_bytes(b'publication metrics fixture\n' * 100)
        ec_port, tcp_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        ip, peer_port = peer.getsockname()
        version = 5
        publish_opcode = 0x44
        reply_opcode = 0x4B
        peer_id = struct.pack('<4I', 0xA0000000, 0, 0, 1)
        contact = peer_id + struct.pack('<IHHB2IB', int.from_bytes(socket.inet_aton(ip), 'big'),
                                       peer_port, peer_port, version, 0, 0, 1)
        (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 1) + contact)
        sections = {'Debug': {'Cat_Kademlia Search': 1}}
        write_amule_conf(root, ec_port, Port=tcp_port, UDPPort=udp_port, ConnectToKad=1,
                         Autoconnect=0, FilterLanIPs=0, VerboseDebug=1, VerboseDebugLogfile=1,
                         sections=sections)
        with running_daemon(binary, root, (ec_port, tcp_port, udp_port), console='console.log') as (proc, ec):
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            # An owned private address enables real LAN-mode source publication, without
            # joining the public DHT or bypassing the production publication policy.
            hello = bytes([0xE4, 0x11]) + peer_id + struct.pack('<HBB', peer_port, version, 0)
            def send_reply(payload, address):
                peer.sendto(payload, address)
            send_reply(hello, ('127.0.0.1', udp_port))
            source_packets = 0
            lookup_packets = 0
            target = None
            # The first self-lookup starts after 180 seconds, then enables publishing
            # after its ordinary 45-second lifetime. Do not accelerate production timers.
            deadline = time.monotonic() + 270
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'daemon exited during publication'
                try:
                    data, address = peer.recvfrom(65535)
                except socket.timeout:
                    data = b''
                if data and address != ('127.0.0.1', udp_port):
                    continue
                if data:
                    if data[0] == 0xE5:
                        data = bytes([0xE4, data[1]]) + zlib.decompress(data[2:])
                    if data[:2] == bytes([0xE4, 0x11]):
                        send_reply(bytes([0xE4, 0x19]) + peer_id + struct.pack('<HBB', peer_port, version, 0), address)
                    elif data[:2] == bytes([0xE4, 0x21]):
                        lookup_packets += 1
                        send_reply(bytes([0xE4, 0x29]) + data[3:19] + bytes(1), address)
                    elif data[:2] == bytes([0xE4, publish_opcode]):
                        source_packets += 1
                        target = data[2:18]
                        reply = bytes([0xE4, reply_opcode]) + target + bytes(1)
                        send_reply(reply, address)
                        send_reply(reply, address) # Duplicate acknowledgements count once.
                lines = (root / 'logfile').read_text(errors='replace').splitlines()
                summaries = [fields(line) for line in lines if 'Kad source publish totals:' in line]
                if summaries and summaries[-1]['acknowledged_ips']:
                    summary = summaries[-1]
                    assert source_packets == 1, source_packets
                    assert summary['queued'] == 1, summary
                    assert summary['acknowledged_ips'] == 1, summary
                    assert summary['attempts'] == summary['starts'] == 1, summary
                    assert summary['start_failures'] == 0, summary
                    # JumpStart exhausts this single-contact lookup, then retains it
                    # for late replies. It naturally finishes before the minute summary.
                    assert summary['finished'] == 1, summary
                    assert summary['zero_ack'] == summary['cancelled'] == 0, summary
                    assert summary['max_req_per_ip_60s'] > 0 and lookup_packets > 0, summary
                    break
            else:
                raise AssertionError(f'No expected Kad2 publication result:\n' + '\n'.join(lines[-100:]))
            assert target is not None
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
            ec.sock.close()
    print('PASS: Kad2 queued publication, duplicate acknowledgements and totals')


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, str(Path(sys.argv[1]).resolve())) or 0)
