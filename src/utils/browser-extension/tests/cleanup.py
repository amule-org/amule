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

"""Stop only recorded aMule processes still using this fixture's config."""
import os
from pathlib import Path
import signal

state = Path(os.environ['ED2K_TEST_STATE'])
if (state / 'pids').exists():
    for line in (state / 'pids').read_text().splitlines():
        pid = int(line)
        try:
            args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if os.fsencode(state / 'amule') in args:
                os.kill(pid, signal.SIGTERM)
        except (FileNotFoundError, ProcessLookupError):
            pass
