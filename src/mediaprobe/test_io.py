#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Give the native AVIO test a real inherited file, independent of CTest's stdin."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryFile() as source:
    source.write(bytes(4096))
    source.seek(0)
    result = subprocess.run([str(Path(sys.argv[1]).resolve())], stdin=source,
                            capture_output=True, timeout=10)
if result.returncode:
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(result.returncode)
