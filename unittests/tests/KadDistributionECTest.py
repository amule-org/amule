#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check Kad diagnostics through authenticated EC on an isolated loopback daemon."""
import hashlib
import os
from pathlib import Path
import struct
import socket
import subprocess
import sys
import tempfile
from AllSearchIntegrationTest import C, connect_daemon, free_port, tag


def local_peer():
    # Bind to an address owned by this machine; never send to external fixture IPs.
    try:
        addresses = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM)
    except OSError:
        return None
    for address in addresses:
        ip = address[4][0]
        octets = tuple(map(int, ip.split('.')))
        if octets[0] in (10, 127) or octets[:2] == (192, 168) or (octets[0] == 172 and 16 <= octets[1] <= 31):
            continue
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            peer.bind((ip, 0))
            return peer
        except OSError:
            peer.close()
    return None


def run(binary, populated=False):
    peer = local_peer() if populated else None
    if populated and peer is None:
        print('No owned non-loopback IPv4 address available')
        return 77
    with tempfile.TemporaryDirectory(prefix='amule-kad-diagnostics-') as directory:
        root = Path(directory)
        port = free_port()
        if peer:
            ip, udp = peer.getsockname()
            contact = struct.pack('<4IIHHB2IB', 0xA0000000, 0, 0, 1,
                                  int.from_bytes(socket.inet_aton(ip), 'big'), udp, udp, 8, 0, 0, 1)
            (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 2, 1) + contact)
        (root / 'amule.conf').write_text(f"""[eMule]
Nick=regression
Port={free_port()}
UDPPort={free_port()}
Address=127.0.0.1
ConnectToKad=1
Autoconnect=0
ConnectToED2K=0
FilterLanIPs=0
NewVersionCheck=0
GeoIPEnabled=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
TempDir={root}/Temp
IncomingDir={root}/Incoming
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
""")
        env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'xdg'))
        with (root / 'stdout.log').open('w') as log:
            proc = subprocess.Popen([binary, '-c', str(root)], stdout=log, stderr=log, env=env)
            try:
                ec = connect_daemon(proc, port)

                assert C['EC_TAG_STATS_KAD_DISTRIBUTION'] not in ec.call(C['EC_OP_STAT_REQ'])[1]

                def distribution(running=False):
                    op, tags = ec.call(C['EC_OP_STAT_REQ'], [tag(C['EC_TAG_STATS_KAD_DISTRIBUTION'])])
                    assert op == C['EC_OP_STATS'], op
                    wire = tags[C['EC_TAG_STATS_KAD_DISTRIBUTION']][0]
                    assert len(wire) >= 12 and wire[0] == 2
                    flags, local_id, subnets, entries = struct.unpack('!BIIH', wire[1:12])
                    assert flags == int(running) and len(wire) == 12 + entries * 10
                    bins, verified = [0] * 4096, [0] * 4096
                    for offset in range(12, len(wire), 10):
                        index, count, checked = struct.unpack('!HII', wire[offset:offset + 10])
                        assert index < 4096 and count and checked <= count
                        bins[index], verified[index] = count, checked
                    return bins, verified, subnets
                assert sum(distribution()[0]) == 0
                reply = ec.call(C['EC_OP_KAD_START'])
                assert reply[0] == C['EC_OP_NOOP'], reply
                bins, verified, subnets = distribution(True)
                expected = 1 if peer else 0
                assert sum(bins) == expected, bins
                assert sum(verified) == expected, verified
                assert subnets == expected, subnets
                if peer:
                    assert bins[2560] == 1 and verified[2560] == 1, (bins, verified)
                # A fresh authenticated client must receive the same requested snapshot.
                ec.sock.close()
                ec = connect_daemon(proc, port)
                assert distribution(True) == (bins, verified, subnets)
                assert C['EC_TAG_STATS_KAD_DISTRIBUTION'] not in ec.call(C['EC_OP_STAT_REQ'])[1]
                assert ec.call(C['EC_OP_KAD_STOP'])[0] == C['EC_OP_NOOP']
                assert sum(distribution()[0]) == 0
                ec.sock.close()
            except BaseException:
                log.flush()
                print((root / 'stdout.log').read_text(), file=sys.stderr)
                if (root / 'logfile').exists():
                    print((root / 'logfile').read_text(), file=sys.stderr)
                raise
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                if peer:
                    peer.close()


if __name__ == '__main__':
    sys.exit(run(str(Path(sys.argv[1]).resolve()), '--populated' in sys.argv) or 0)
