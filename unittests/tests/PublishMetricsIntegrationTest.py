#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe real source publication, transport accounting and duplicate replies on a private LAN peer."""
from contextlib import ExitStack
import ipaddress
import re
import socket
import struct
import sys
import tempfile
import time
import threading
import zlib
from pathlib import Path
from AllSearchIntegrationTest import C, exact, free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from SharedFileReverifyIntegrationTest import records


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


class LocalSocks5Relay:
    """A local UDP ASSOCIATE endpoint; no packet is forwarded to an external host."""
    def __init__(self):
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', 0))
        self.udp.settimeout(0.1)
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def serve(self):
        try:
            while not self.stop.is_set():
                try:
                    connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            with connection:
                connection.settimeout(5)
                greeting = exact(connection, 2)
                assert greeting[0] == 5
                assert 0 in exact(connection, greeting[1])
                connection.sendall(bytes([5, 0]))
                assert exact(connection, 4) == bytes([5, 3, 0, 1])
                exact(connection, 6) # Client's UDP endpoint.
                connection.sendall(bytes([5, 0, 0, 1]) + socket.inet_aton('127.0.0.1')
                                   + struct.pack('!H', self.udp.getsockname()[1]))
                self.ready.set()
                self.stop.wait()
        except Exception as error:
            if not self.stop.is_set():
                self.error = error
                self.ready.set()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, kind, error, traceback):
        self.stop.set()
        self.thread.join(6)
        self.udp.close()
        self.listener.close()
        if kind is None and self.error:
            raise self.error


def fields(line):
    return {key: int(value) for key, value in re.findall(r'\b(\w+)=(\d+)(?=\s|$)', line)}


def run(binary, mode="kad2"):
    peer = local_peer()
    if peer is None:
        print('No owned private non-loopback IPv4 address for isolated Kad peer')
        return 77
    with peer, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as rogue, ExitStack() as stack, \
            tempfile.TemporaryDirectory(prefix='amule-publish-metrics-') as directory:
        root = Path(directory)
        incoming = root / 'Incoming'
        incoming.mkdir()
        (incoming / 'publication.bin').write_bytes(b'publication metrics fixture\n' * 100)
        ec_port, tcp_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        ip, peer_port = peer.getsockname()
        version = 3 if mode == "kad1" else 5
        publish_opcode = 0x40 if mode == "kad1" else 0x44
        reply_opcode = 0x48 if mode == "kad1" else 0x4B
        peer_id = struct.pack('<4I', 0xA0000000, 0, 0, 1)
        contact = peer_id + struct.pack('<IHHB2IB', int.from_bytes(socket.inet_aton(ip), 'big'),
                                       peer_port, peer_port, version, 0, 0, 1)
        (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 1) + contact)
        relay = stack.enter_context(LocalSocks5Relay()) if mode == "proxy" else None
        sections = {'Debug': {'Cat_Kademlia Search': 1}}
        if relay:
            sections['Proxy'] = dict(ProxyEnableProxy=1, ProxyType=0, ProxyName='127.0.0.1',
                                     ProxyPort=relay.listener.getsockname()[1])
        write_amule_conf(root, ec_port, Port=tcp_port, UDPPort=udp_port, ConnectToKad=1,
                         Autoconnect=0, FilterLanIPs=0, VerboseDebug=1, VerboseDebugLogfile=1,
                         sections=sections)
        with running_daemon(binary, root, (ec_port, tcp_port, udp_port), console='console.log') as (proc, ec):
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            # An owned private address enables real LAN-mode source publication, without
            # joining the public DHT or bypassing the production publication policy.
            hello = bytes([0xE4, 0x11]) + peer_id + struct.pack('<HBB', peer_port, version, 0)
            def send_reply(payload, address):
                if relay:
                    wrapper = bytes([0, 0, 0, 1]) + socket.inet_aton(ip) + struct.pack('!H', peer_port)
                    relay.udp.sendto(wrapper + payload, ('127.0.0.1', udp_port))
                else:
                    peer.sendto(payload, address)
            if relay:
                assert relay.ready.wait(10), 'SOCKS5 association was not requested'
                assert relay.error is None, relay.error
                time.sleep(0.2) # Let the core consume the association reply.
            send_reply(hello, ('127.0.0.1', udp_port))
            source_packets = 0
            lookup_packets = 0
            sent_bytes = 0
            target = None
            removed = False
            if mode == "wrong-port":
                rogue.bind((ip, 0))
            # The first self-lookup starts after 180 seconds, then enables publishing
            # after its ordinary 45-second lifetime. Do not accelerate production timers.
            deadline = time.monotonic() + 270
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'daemon exited during publication'
                try:
                    data, address = (relay.udp if relay else peer).recvfrom(65535)
                except socket.timeout:
                    data = b''
                if data and address != ('127.0.0.1', udp_port):
                    continue
                if data:
                    native_bytes = len(data)
                    if relay:
                        assert data[:4] == bytes([0, 0, 0, 1]), data
                        assert data[4:8] == socket.inet_aton(ip), data
                        assert struct.unpack('!H', data[8:10])[0] == peer_port, data
                        data = data[10:]
                    sent_bytes += native_bytes if data[1] == publish_opcode else 0
                    if data[0] == 0xE5:
                        data = bytes([0xE4, data[1]]) + zlib.decompress(data[2:])
                    if data[:2] == bytes([0xE4, 0x11]):
                        send_reply(bytes([0xE4, 0x19]) + peer_id + struct.pack('<HBB', peer_port, version, 0), address)
                    elif data[:2] == bytes([0xE4, 0x21]):
                        lookup_packets += 1
                        if mode == "removed" and not removed:
                            log = (root / 'logfile').read_text(errors='replace')
                            if 'Kad source publish attempt:' in log:
                                (incoming / 'publication.bin').unlink()
                                assert ec.call(C['EC_OP_SHAREDFILES_RELOAD'])[0] == C['EC_OP_NOOP']
                                stop = time.monotonic() + 10
                                while records(ec):
                                    assert proc.poll() is None
                                    assert time.monotonic() < stop, 'file was not unshared'
                                    time.sleep(0.05)
                                removed = True
                        send_reply(bytes([0xE4, 0x29]) + data[3:19] + bytes(1), address)
                    elif data[:2] == bytes([0xE4, publish_opcode]):
                        source_packets += 1
                        target = data[2:18]
                        reply = bytes([0xE4, reply_opcode]) + target + bytes(1)
                        if mode == "wrong-port":
                            rogue.sendto(reply, address)
                        else:
                            send_reply(reply, address)
                            send_reply(reply, address) # Two replies are one accepting destination IP.
                lines = (root / 'logfile').read_text(errors='replace').splitlines()
                summaries = [fields(line) for line in lines if 'Kad source publish totals:' in line]
                if mode == "wrong-port" and any('Kad source publish reply:' in line for line in lines):
                    assert source_packets == 1
                    break
                if mode == "removed" and summaries and summaries[-1]['finished']:
                    summary = summaries[-1]
                    assert removed and source_packets == 0, (removed, source_packets, summary)
                    assert summary['attempts'] == summary['starts'] == summary['finished'] == summary['zero_ack'] == 1, summary
                    assert summary['sent'] == summary['queued'] == summary['acknowledged_ips'] == 0, summary
                    assert summary['in_flight'] == summary['cancelled'] == 0, summary
                    break
                if mode in ("kad1", "kad2", "proxy") and summaries and summaries[-1]['acknowledged_ips']:
                    summary = summaries[-1]
                    assert source_packets == 1, source_packets
                    assert summary['queued'] == summary['sent'] == 1, summary
                    assert summary['sent_bytes'] == sent_bytes, (summary, sent_bytes)
                    assert summary['acknowledged_ips'] == 1, summary
                    assert summary['attempts'] == summary['starts'] == 1, summary
                    assert summary['start_failures'] == summary['send_failures'] == 0, summary
                    # JumpStart exhausts this single-contact lookup, then retains it
                    # for late replies. It naturally finishes before the minute summary.
                    assert summary['finished'] == 1 and summary['in_flight'] == 0, summary
                    assert summary['zero_ack'] == summary['cancelled'] == 0, summary
                    assert summary['max_req_per_ip_60s'] > 0 and lookup_packets > 0, summary
                    break
            else:
                raise AssertionError(f'No expected {mode} publication result:\n' + '\n'.join(lines[-100:]))
            assert target is not None or mode == "removed"
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
            # Stopping Kad must archive the source trace, with its accepted destination count.
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                lines = (root / 'logfile').read_text(errors='replace').splitlines()
                completed = [fields(line) for line in lines if 'Kad source publish: target=' in line]
                if completed:
                    expected = 1 if mode in ("kad1", "kad2", "proxy") else 0
                    assert completed[-1]['acknowledged_ips'] == expected, completed
                    assert completed[-1]['completed'] == int(mode != "wrong-port"), completed
                    assert completed[-1]['cancelled'] == int(mode == "wrong-port"), completed
                    assert completed[-1]['duration_ms'] > 0, completed
                    break
                time.sleep(0.05)
            else:
                raise AssertionError('Missing source publication lifecycle record')
            # Repeated stop/restart cannot finish the same source trace a second time.
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
            again = (root / 'logfile').read_text(errors='replace')
            assert sum('Kad source publish: target=' in line for line in again.splitlines()) == 1
            ec.sock.close()
    print(f'PASS: {mode} publication, native send accounting, unique acknowledgements and lifecycle logs')


if __name__ == '__main__':
    sys.exit(retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), sys.argv[2] if len(sys.argv) > 2 else 'kad2') or 0)
