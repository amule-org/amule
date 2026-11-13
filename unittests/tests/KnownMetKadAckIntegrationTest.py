#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""known.met keeps when nodes last acknowledged a file's Kad source entry, and how many.

The Diagnostics count of files published on Kad reads both, so a restart must not lose them. A
time ahead of the clock is corrupt: it is dropped on load and not saved again.
"""
from pathlib import Path
import re
import struct
import sys
import tempfile
import time

from AllSearchIntegrationTest import free_ports, retry_port_collisions, running_daemon
from DaemonConfig import write_amule_conf
from KnownHistoryIntegrationTest import known_met, known_record, wait_for

ACKED = bytes(range(32, 48))
FUTURE = bytes(range(48, 64))
FT_KADSRCACKTIME, FT_KADSRCACKCOUNT = 0x5B, 0x5C


def uint32_tag(name_id, value):
    """An integer tag as CFileDataIO::WriteTag writes it: type, a one-byte name, the value."""
    return b'\x03' + struct.pack('<H', 1) + bytes([name_id]) + struct.pack('<I', value)


def run(binary, root):
    for d in ('Incoming', 'Temp'):
        (root / d).mkdir()
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    ec_port, peer_port, udp_port = ports
    write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port)
    now = int(time.time())
    acked = (uint32_tag(FT_KADSRCACKTIME, now - 600), uint32_tag(FT_KADSRCACKCOUNT, 6))
    future = (uint32_tag(FT_KADSRCACKTIME, now + 3600), uint32_tag(FT_KADSRCACKCOUNT, 6))
    (root / 'known.met').write_bytes(known_met(
        known_record(ACKED, 'acked.bin', 4096, last_seen=now, extra_tags=acked),
        known_record(FUTURE, 'future.bin', 4096, last_seen=now, extra_tags=future)))

    with running_daemon(binary, root, ports) as (proc, ec):
        wait_for(proc, 'the first share scan',
                 lambda: re.search(r'Found \d+ known shared file', (root / 'logfile').read_text(errors='replace')))
    saved = (root / 'known.met').read_bytes()
    assert ACKED in saved and FUTURE in saved, 'a record was dropped'
    for tag in acked:
        assert tag in saved, 'the acknowledgement was not saved'
    assert future[0] not in saved, 'a time ahead of the clock was saved again'
    assert saved.count(uint32_tag(FT_KADSRCACKCOUNT, 6)) == 1, 'a count was saved without its time'


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-known-ack-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    print('Kad source acknowledgements survive a restart')


if __name__ == '__main__':
    main()
