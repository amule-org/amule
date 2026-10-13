#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A link must not carry a line break, or a category the daemon does not have.

A server-list address is URL-decoded, so %0A became a real line break: the browser handler writes
links to ED2KLinks one per line, and that one link forged more. A category index came off the
link or the EC packet and indexed the category list unchecked.
"""
import hashlib
from pathlib import Path
import sys
import tempfile

from AllSearchIntegrationTest import (C, free_ports, retry_port_collisions, stop_daemon, string,
                                      tag)
from KnownHistoryIntegrationTest import start

BIG = 5000000000  # over 4 GiB, so the Incoming file system is checked first


def add_link(ec, link, category=None):
    children = [tag(C['EC_TAG_PARTFILE_CAT'], bytes([category]), 2)] if category is not None else []
    return ec.call(C['EC_OP_ADD_LINK'], [tag(C['EC_TAG_STRING'], link.encode() + b'\0', 6, children)])[0]


def run(binary, root):
    for d in ('Incoming', 'Temp'):
        (root / d).mkdir()
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    ec_port, peer_port, udp_port = ports
    (root / 'amule.conf').write_text(f'''[eMule]
Nick=ed2k-link-regression
FirstRunWizardDone=1
Port={peer_port}
UDPPort={udp_port}
Address=127.0.0.1
ConnectToKad=0
ConnectToED2K=1
NewVersionCheck=0
GeoIPEnabled=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
TempDir={root}/Temp
IncomingDir={root}/Incoming
# The checks match English log lines; LC_ALL alone does not override a translated LANG.
Language=en_US
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={ec_port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
''')

    proc, ec = start(binary, root, ports)
    try:
        forged = 'ed2k://|serverlist|http://127.0.0.1:9/a%0Aed2k://|server|1.2.3.4|4661|/|/'
        assert add_link(ec, forged) == C['EC_OP_FAILED'], 'a server-list address with a line break was accepted'
        assert add_link(ec, 'ed2k://|serverlist|http://127.0.0.1:9/server.met|/') == C['EC_OP_NOOP']

        # Only category 0 exists.
        assert add_link(ec, f'ed2k://|file|big.bin|{BIG}|{"0123456789ABCDEF" * 2}|/', 200) == C['EC_OP_NOOP']
        log = (root / 'logfile').read_text(errors='replace')
        assert 'Downloading big.bin' in log, log[-2000:]
        assert 'GetCatPath' not in log, 'an out-of-range category reached GetCatPath'
        ec.sock.close()
    finally:
        stop_daemon(proc)


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-ed2k-link-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    print('ED2K link checks passed')


if __name__ == '__main__':
    main()
