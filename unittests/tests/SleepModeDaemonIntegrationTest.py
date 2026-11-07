#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise sleep-mode loading, saving and restart in an offline, empty daemon.

SleepInhibitionTest supplies a private bus and checks acquisition/release calls.
"""
import configparser
from pathlib import Path
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, free_ports, integer, retry_port_collisions, running_daemon
from DaemonConfig import daemon_env, write_amule_conf


def run(binary, new_mode, legacy, expected, scenario):
    with tempfile.TemporaryDirectory(prefix='amule-sleep-mode-') as temporary:
        root = Path(temporary)
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        power = {key: value for key, value in (
            ('PreventSleep', new_mode), ('PreventSleepWhileDownloading', legacy))
                 if value != 'missing'}
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2],
                         sections={'PowerManagement': power})
        # Inherits the isolated bus from the C++ parent, never the user's desktop bus.
        env = daemon_env(root)
        for _ in range(2):
            started = time.monotonic()
            with running_daemon(binary, root, ports, env=env) as (_, ec):
                deadline = time.monotonic() + 15
                while True:
                    op, tags = ec.call(C['EC_OP_STAT_REQ'], [
                        integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_FULL'])])
                    assert op == C['EC_OP_STATS'], op
                    if tags[C['EC_TAG_STATS_UPTIME_SECONDS']][0] >= 1:
                        break
                    assert time.monotonic() < deadline, 'core timer did not run'
                    time.sleep(0.05)
            if scenario == 'held':
                assert time.monotonic() - started < 3, 'desktop inhibition stalled the core or shutdown'
            config = configparser.ConfigParser(interpolation=None)
            config.optionxform = str
            config.read(root / 'amule.conf')
            assert config.getint('PowerManagement', 'PreventSleep') == expected
            assert config.getboolean('PowerManagement', 'PreventSleepWhileDownloading') == bool(expected)
        print(f'PASS: new={new_mode}, legacy={legacy}, saved={expected}')


if __name__ == '__main__':
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve()),
                                    sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]))
