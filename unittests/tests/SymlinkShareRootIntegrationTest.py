#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A share root named through a symlink stays one root.

amuled stores roots resolved. A client that matches roots by string re-sends the stored
form next to the symlinked one it was given, and the folder must not end up in both the
explicit and the recursive list.
"""
from pathlib import Path
import os
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, running_daemon, tag
from CompletedFileDeleteIntegrationTest import EC
from DaemonConfig import write_amule_conf


def root_tag(path, recursive):
    children = [tag(C['EC_TAG_SHAREDDIR_RECURSIVE'], b'\x01', 2)] if recursive else []
    return tag(C['EC_TAG_SHAREDDIR'], os.fsencode(path) + b'\0', 6, children)


def set_roots(ec, *roots):
    op, tags = ec.call('EC_OP_SET_SHARED_DIRS', [root_tag(p, r) for p, r in roots])
    assert op == C['EC_OP_SET_SHARED_DIRS'], (op, tags)
    assert not any(name == C['EC_TAG_SHAREDDIR_REJECTED'] for name, _, _ in tags), tags


def roots(ec):
    _, tags = ec.call('EC_OP_GET_SHARED_DIRS')
    return sorted((value.rstrip(b'\0').decode(),
                   any(name == C['EC_TAG_SHAREDDIR_RECURSIVE'] for name, _, _ in children))
                  for name, value, children in tags if name == C['EC_TAG_SHAREDDIR'])


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-symlink-share-root-') as scratch:
        root = Path(scratch).resolve()
        real = root / 'real'
        real.mkdir()
        link = root / 'link'
        link.symlink_to(real, target_is_directory=True)
        (root / 'allowed-remote-dirs.dat').write_text(f'{root}\n')
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0)
        with running_daemon(binary, root, (ec_port, peer_port), ec_class=EC) as (_, ec):
            set_roots(ec, (link, True))
            assert roots(ec) == [(str(real), True)], roots(ec)
            print('PASS: a root added through a symlink is stored resolved')

            # What a string-matching client sends to turn recursion off for that root.
            set_roots(ec, (real, True), (link, False))
            assert roots(ec) == [(str(real), False)], roots(ec)
            print('PASS: re-sending it through the symlink changes the one root')

            set_roots(ec, (link, True))
            assert roots(ec) == [(str(real), True)], roots(ec)
            print('PASS: the symlinked form alone matches the stored root')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
