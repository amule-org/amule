#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise AICH prune commits and corruption recovery with an offline daemon."""
import base64
from pathlib import Path
import struct
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, start_daemon, stop_daemon
from DaemonConfig import write_amule_conf


def run(binary, corrupt=False):
    with tempfile.TemporaryDirectory(prefix='amule-aich-prune-') as directory:
        root = Path(directory)
        (root / 'Incoming').mkdir()
        (root / 'Temp').mkdir()
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0)
        master = bytes(range(20))
        orphan = bytes(range(20, 40))
        name = b'known-but-unshared.bin'
        tags = (b'\x82\x01' + struct.pack('<H', len(name)) + name
                + b'\x83\x02' + struct.pack('<I', 64)
                + b'\x82\x27\x20\x00' + base64.b32encode(master))
        record = (struct.pack('<I', int(time.time())) + bytes(range(16))
                  + struct.pack('<HI', 0, 3) + tags)
        (root / 'known.met').write_bytes(b'\x0e' + struct.pack('<I', 1) + record)
        live_record = master + struct.pack('<I', 1) + master
        orphan_record = orphan + struct.pack('<I', 1) + orphan
        expected = b'\x02' + live_record
        aich = root / 'known2_64.met'
        if corrupt:
            # Keep a complete orphan before an incomplete live record. Recovery must
            # retain the complete source prefix, rather than install the empty prune.
            aich.write_bytes(b'\x02' + orphan_record + live_record[:-1])
            expected = b'\x02' + orphan_record
        else:
            aich.write_bytes(expected + orphan_record)
        proc = ec = None
        try:
            proc, ec = start_daemon(binary, root, (ec_port, peer_port), args=(),
                                    console='stdout.log')
            # The source reader must be closed before committing the sibling .new.
            # Otherwise Windows rejects the native replacement with a sharing error.
            deadline = time.monotonic() + 30
            while aich.read_bytes() != expected or (root / 'known2_64.met.new').exists():
                if corrupt:
                    assert aich.read_bytes() != b'\x02', 'partial prune replaced recovered source'
                assert proc.poll() is None, 'daemon exited during AICH prune'
                assert time.monotonic() < deadline, 'orphan AICH hashset was not pruned'
                time.sleep(0.05)
            assert not (root / 'known2_64.met.new').exists(), 'temporary rewrite remains after completion'
            # Request normal shutdown through EC on every platform. terminate()
            # forcibly kills a Windows process before it can save or close files.
            ec.sock.sendall(struct.pack('!II', 0x20, 3) + bytes([C['EC_OP_SHUTDOWN'], 0, 0]))
            assert proc.wait(timeout=30) == 0, 'daemon shutdown failed'
            ec.sock.close()
            ec = None
            assert aich.read_bytes() == expected, 'shutdown changed the retained hashset'
            print('AICH corruption recovery discarded its partial rewrite' if corrupt else
                  'AICH orphan prune preserved the live hashset and committed its replacement')
        except BaseException:
            for name in ('stdout.log', 'logfile'):
                path = root / name
                if path.exists():
                    print(path.read_text(errors='replace')[-6000:], file=sys.stderr)
            raise
        finally:
            if ec:
                ec.sock.close()
            if proc:
                stop_daemon(proc, check=False)


if __name__ == '__main__':
    for corrupt in (False, True):
        retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), corrupt)
