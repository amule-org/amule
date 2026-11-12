#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check timestamp-first files/stdout and legacy EC markers on an isolated offline daemon."""
from datetime import datetime
from pathlib import Path
import re
import subprocess
import tempfile

from AllSearchIntegrationTest import (C, DAEMON_ARGS, free_ports, retry_port_collisions, running_daemon,
                                      string, tag)
from DaemonConfig import daemon_env, write_amule_conf

STAMP = r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}'
# Legacy macros include a source basename and line number in Debug builds.
SOURCE = r'(?:[^()\r\n]+\(\d+\): )?'


def run(binary, amulecmd=None):
    with tempfile.TemporaryDirectory(prefix='amule-logging-format-') as directory:
        root = Path(directory)
        port, peer_port = free_ports(2)
        write_amule_conf(root, port, Port=peer_port, UDPEnable=0)
        env = daemon_env(root)
        with running_daemon(binary, root, (port, peer_port), args=(*DAEMON_ARGS, '--log-stdout'),
                            env=env, stop_timeout=15) as (_, ec):
            for status, message in [(False, 'logging-format-ordinary café'),
                                    (True, 'logging-format-status café'),
                                    (False, 'logging-format-multiline café\n\nlast line')]:
                # Drain startup/previous records before checking incremental replay.
                ec.call(C['EC_OP_STAT_REQ'])
                tags = [string(C['EC_TAG_STRING'], message)]
                if status:
                    tags.append(tag(C['EC_TAG_LOG_TO_STATUS']))
                assert ec.call(C['EC_OP_ADDLOGLINE'], tags)[0] == C['EC_OP_NOOP']
                op, stats = ec.call(C['EC_OP_STAT_REQ'])
                assert op == C['EC_OP_STATS'], op
                incremental = stats[C['EC_TAG_STATS_LOGGER_MESSAGE']][1][
                    C['EC_TAG_STRING']][0].decode('utf-8')
                op, debug_tags = ec.call(C['EC_OP_GET_DEBUGLOG'])
                assert op == C['EC_OP_DEBUGLOG'], op
                debug_snapshot = debug_tags[C['EC_TAG_STRING']][0].decode('utf-8')
                op, tags = ec.call(C['EC_OP_GET_LOG'])
                replay = tags[C['EC_TAG_STRING']][0].decode('utf-8')
                assert op == C['EC_OP_LOG'], op
                # C still means status-bar visibility, and retains INFO severity.
                marker = '!' if status else ' '
                disk = (root / 'logfile').read_text(encoding='utf-8')
                stdout = (root / 'console.log').read_text(encoding='utf-8')
                for physical_line in message.split('\n'):
                    pattern = re.compile('(' + STAMP + ')' + r' INFO  '
                                         + rf'\[log status={int(status)} replay=1\] '
                                         + SOURCE + re.escape(physical_line))
                    match = next((pattern.fullmatch(line) for line in disk.splitlines()
                                  if pattern.fullmatch(line)), None)
                    assert match is not None, disk
                    human = re.sub(r'\[log status=[01] replay=[01]\] ', '', match[0], count=1)
                    assert marker + human in replay.splitlines(), replay
                    assert marker + human in debug_snapshot.splitlines(), debug_snapshot
                    if physical_line == message.split("\n")[-1]:
                        assert marker + human in incremental.splitlines(), incremental
                    assert human in stdout.splitlines(), stdout
                    assert all(re.match(STAMP, line) for line in disk.splitlines()), disk
                    instant = datetime.strptime(match[1], '%Y-%m-%dT%H:%M:%S.%f%z')
                    now = datetime.now().astimezone()
                    assert instant.utcoffset() == now.utcoffset(), (instant, now)
                    assert abs((now - instant).total_seconds()) < 30, (instant, now)
            if amulecmd:
                result = subprocess.run([amulecmd, '-h', '127.0.0.1', '-p', str(port),
                                         '-P', 'regression', '--command=show log'],
                                        env=env, capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, (result.stdout, result.stderr)
                lines = [line for line in result.stdout.splitlines() if 'logging-format-' in line]
                assert len(lines) >= 3, result.stdout
                assert all(re.match(STAMP, line) for line in lines), result.stdout
                assert '[log status=' not in result.stdout, result.stdout
    print('LoggingFormatIntegrationTest: stdout, logfile and legacy EC replay passed')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--amulecmd')
    args = parser.parse_args()
    retry_port_collisions(lambda: run(str(Path(args.binary).resolve()), args.amulecmd))
