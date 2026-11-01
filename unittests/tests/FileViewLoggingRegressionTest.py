#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep fileview's shared file parser safe when disabled debug checks run in Release."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from DaemonConfig import daemon_env


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-fileview-logging-') as folder:
        env = daemon_env(folder)
        path = Path(folder) / 'src_index.dat'
        header = struct.pack('<III', 1, 0, 1)
        source = bytes(16) + struct.pack('<I', 1) + bytes(16)
        record = header + source + struct.pack('<IIB', 1, 0, 1)
        cases = [
            (struct.pack('<III', 1, 0, 0), 0, 'numKeys : 0'),
            (record + b'\x7f\x01\x00x', 1, 'Invalid Kad tag type'),
            (record + b'\x02', 1, 'CEOFException'),
        ]
        for payload, expected, message in cases:
            path.write_bytes(payload)
            result = subprocess.run([binary, str(path)], env=env, capture_output=True,
                                    text=True, errors='replace', check=False)
            output = result.stdout + result.stderr
            assert result.returncode == expected, (result.returncode, output)
            assert message in output, output
            assert 'AddressSanitizer' not in output and 'runtime error:' not in output, output
            assert 'SafeFile.cpp(' not in output, 'disabled debug message reached stdout: ' + output
    print('fileview logging regression checks passed')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
