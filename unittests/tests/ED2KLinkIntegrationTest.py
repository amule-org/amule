#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A link must not carry a line break, or a category the daemon does not have.

A server-list address is URL-decoded, so %0A became a real line break: the browser handler writes
links to ED2KLinks one per line, and that one link forged more. A category index came off the
link or the EC packet and indexed the category list unchecked. The ed2k helper writes ED2KLinks
too, from its decoded argument.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from AllSearchIntegrationTest import (C, free_ports, retry_port_collisions, stop_daemon, string,
                                      tag)
from DaemonConfig import write_amule_conf
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
    write_amule_conf(root, ec_port, Port=peer_port, UDPPort=udp_port, ConnectToED2K=1)

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


def check_helper(helper):
    """The ed2k helper decodes its argument and writes ED2KLinks itself."""
    link = f'ed2k://|file|helper.bin|1048576|{"0123456789ABCDEF" * 2}|/'
    with tempfile.TemporaryDirectory(prefix='amule-ed2k-helper-') as scratch:
        root = Path(scratch).resolve()
        links = root / 'ED2KLinks'
        # -c first: arguments are handled in order.
        forged = subprocess.run([helper, '-c', str(root), link + '%0Aed2k://|server|1.2.3.4|4661|/'],
                                capture_output=True, timeout=30)
        assert forged.returncode != 0, forged
        assert not links.exists() or '1.2.3.4' not in links.read_text(), links.read_text()
        plain = subprocess.run([helper, '-c', str(root), link], capture_output=True, timeout=30)
        assert plain.returncode == 0, plain
        assert links.read_text().splitlines() == [link], links.read_text()


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-ed2k-link-') as scratch:
            run(binary, Path(scratch).resolve())
    retry_port_collisions(attempt)
    if len(sys.argv) > 2:
        check_helper(str(Path(sys.argv[2]).resolve()))
    print('ED2K link checks passed')


if __name__ == '__main__':
    main()
