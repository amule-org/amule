#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify amuled reports a missing media helper and refuses media re-extraction over EC."""
from pathlib import Path
import shutil
import sys
import tempfile

from AllSearchIntegrationTest import (C, free_ports, integer, log_snapshot, new_log_text,
                                      retry_port_collisions, running_daemon, tag)
from DaemonConfig import daemon_env, write_amule_conf

UNAVAILABLE_LOG = 'Media metadata: bundled amule-media-probe helper is unavailable.'
UNAVAILABLE_EC = b'This aMule has no media helper (amule-media-probe)'


def refusal(tags):
    return tags.get(C['EC_TAG_STRING'], (b'',))[0].rstrip(b'\0')


def run(daemon_binary, helper_binary, with_helper):
    with tempfile.TemporaryDirectory(prefix='amule-media-helper-') as temporary:
        root = Path(temporary)
        core, bin_dir = root / 'core', root / 'bin'
        core.mkdir()
        bin_dir.mkdir()
        # A copy, so the core's lookup beside itself and at ../libexec/amule finds only
        # what the test puts there.
        daemon = shutil.copy2(daemon_binary, bin_dir / Path(daemon_binary).name)
        if with_helper:
            shutil.copy2(helper_binary, bin_dir / Path(helper_binary).name)
        ec_port, peer_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(core, ec_port, Port=peer_port, UDPPort=udp_port,
                         sections={'MediaMetadata': {'Enabled': 1}})
        snapshot = log_snapshot(core / 'logfile')
        with running_daemon(daemon, core, (ec_port, peer_port, udp_port),
                            env=daemon_env(root)) as (proc, ec):
            startup = new_log_text(core / 'logfile', snapshot)
            assert (UNAVAILABLE_LOG in startup) is not with_helper, startup

            _, tags = ec.call(C['EC_OP_GET_PREFERENCES'],
                              [integer(C['EC_TAG_SELECT_PREFS'], C['EC_PREFS_FILES'])])
            files = tags[C['EC_TAG_PREFS_FILES']][1]
            available = files[C['EC_TAG_FILES_MEDIA_METADATA_AVAILABLE']][0]
            assert available == int(with_helper), files

            op, tags = ec.call(C['EC_OP_REFRESH_MEDIA_METADATA'])
            if with_helper:
                assert op == C['EC_OP_NOOP'], (op, tags)
                assert C['EC_TAG_KNOWNFILE_MEDIA_QUEUED'] in tags, tags
            else:
                assert op == C['EC_OP_FAILED'] and refusal(tags) == UNAVAILABLE_EC, (op, tags)
            # One file: refused for the helper before the hash is looked up.
            op, tags = ec.call(C['EC_OP_REFRESH_MEDIA_METADATA'],
                               [tag(C['EC_TAG_KNOWNFILE'], bytes(16), 9)])
            assert op == C['EC_OP_FAILED'], (op, tags)
            assert (refusal(tags) == UNAVAILABLE_EC) is not with_helper, tags
        print('PASS:', 'with' if with_helper else 'without', 'helper: available =',
              available, '; re-extract', 'accepted' if with_helper else 'refused')


if __name__ == '__main__':
    for with_helper in (False, True):
        retry_port_collisions(run, sys.argv[1], sys.argv[2], with_helper)
