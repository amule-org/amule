#!/usr/bin/env python3
#
# This file is part of the aMule Project.
#
# Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
#

"""Log native protocol delivery, optionally forwarding it to isolated aMule."""
import json
from pathlib import Path
import subprocess
import sys

# This executable is snapshotted under state/bin. Desktop launchers need not
# preserve arbitrary fixture environment variables.
state = Path(__file__).resolve().parents[1]
settings = json.loads((state / 'runtime.json').read_text())
with (state / 'handler.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\n')
if settings['binary']:
    with (state / 'amule-stdout.log').open('ab') as stream:
        process = subprocess.Popen(
            [settings['binary'], '-c', str(state / 'amule'), '-o', *sys.argv[1:]],
            stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
            start_new_session=True)
    with (state / 'pids').open('a') as stream:
        stream.write(str(process.pid) + '\n')
