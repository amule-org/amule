#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check stdout and legacy EC/logfile markers on an isolated offline daemon."""
from datetime import datetime
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from AllSearchIntegrationTest import (C, connect_daemon, free_ports,
                                      retry_port_collisions, string, tag)
from DaemonConfig import write_amule_conf

STAMP = r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}'
# Legacy macros include a source basename and line number in Debug builds.
SOURCE = r'(?:[^()\r\n]+\(\d+\): )?'


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-logging-format-') as directory:
        root = Path(directory)
        port, peer_port = free_ports(2)
        write_amule_conf(root, port, Port=peer_port, UDPEnable=0)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'),
                   LC_ALL='C.UTF-8')
        with (root / 'console.log').open('w') as console:
            proc = subprocess.Popen([binary, '-c', str(root), '--log-stdout', '--disable-fatal'],
                                    stdout=console, stderr=console, env=env)
            ec = None
            try:
                ec = connect_daemon(proc, port, root, peer_port)
                for status, message in [(False, 'logging-format-ordinary café'),
                                        (True, 'logging-format-status café'),
                                        (False, 'logging-format-multiline café\n\nlast line')]:
                    tags = [string(C['EC_TAG_STRING'], message)]
                    if status:
                        tags.append(tag(C['EC_TAG_LOG_TO_STATUS']))
                    assert ec.call(C['EC_OP_ADDLOGLINE'], tags)[0] == C['EC_OP_NOOP']
                    op, tags = ec.call(C['EC_OP_GET_LOG'])
                    replay = tags[C['EC_TAG_STRING']][0].decode('utf-8')
                    assert op == C['EC_OP_LOG'], op
                    # C still means status-bar visibility, and retains INFO severity.
                    marker = '!' if status else ' '
                    disk = (root / 'logfile').read_text(encoding='utf-8')
                    stdout = (root / 'console.log').read_text(encoding='utf-8')
                    for physical_line in message.split('\n'):
                        pattern = re.compile(re.escape(marker) + '(' + STAMP + ')'
                                             + r' INFO  ' + SOURCE + re.escape(physical_line))
                        match = next((pattern.fullmatch(line) for line in disk.splitlines()
                                      if pattern.fullmatch(line)), None)
                        assert match is not None, disk
                        assert match[0] in replay.splitlines(), replay
                        assert match[0][1:] in stdout.splitlines(), stdout
                        assert match[0] not in stdout.splitlines(), stdout
                        instant = datetime.strptime(match[1], '%Y-%m-%dT%H:%M:%S.%f%z')
                        now = datetime.now().astimezone()
                        assert instant.utcoffset() == now.utcoffset(), (instant, now)
                        assert abs((now - instant).total_seconds()) < 30, (instant, now)
            except BaseException:
                console.flush()
                print((root / 'console.log').read_text(errors='replace'), file=sys.stderr)
                raise
            finally:
                if ec:
                    ec.sock.close()
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                assert proc.returncode == 0, proc.returncode
    print('LoggingFormatIntegrationTest: stdout, logfile and legacy EC replay passed')


if __name__ == '__main__':
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve())))
