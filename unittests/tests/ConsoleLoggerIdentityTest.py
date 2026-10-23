#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Console tags come from declared names, independent of the executable path."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def check(binary):
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                            timeout=15)
    lines = result.stdout.splitlines()
    assert len(lines) == 7, result.stdout
    for i, name in enumerate(['amulecmd', 'amuleweb', 'amuleapi']):
        for line in lines[2 * i:2 * i + 2]:
            assert re.search(r' INFO  \[' + name + r'\] (?:probe\(7\): )?console identity$',
                             line), line
    assert lines[-1].endswith(' WARN  [FileIO] explicit subsystem'), lines[-1]


binary = Path(sys.argv[1]).resolve()
check(binary)
with tempfile.TemporaryDirectory(prefix='console-logger-') as directory:
    renamed = Path(directory) / ('renamed-logger' + binary.suffix)
    shutil.copy2(binary, renamed)
    check(renamed)
    if sys.platform != 'win32':
        link = Path(directory) / 'symlinked-logger'
        link.symlink_to(binary)
        check(link)
print('Console logger identity survives repeated records, renaming and symlinking')
