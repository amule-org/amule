#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""remote.conf holds the EC password hash, which is the credential: it must be owner-only.

It used to be written with the umask, and only restricted the next time it was loaded.
"""
import hashlib
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

from DaemonConfig import daemon_env


def mode(path):
    return stat.S_IMODE(path.stat().st_mode)


def run(binary, root, *args):
    subprocess.run([binary, *args], env=daemon_env(root), cwd=root, check=True, timeout=60,
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    found = list(root.rglob('remote.conf'))
    assert len(found) == 1, found
    return found[0]


def main():
    # Only Linux takes the config directory from HOME; elsewhere this would write the real one.
    if not sys.platform.startswith('linux'):
        sys.exit('RemoteConfPermissionsTest only runs on Linux')
    binary = str(Path(sys.argv[1]).resolve())
    os.umask(0o022)
    with tempfile.TemporaryDirectory(prefix='amule-remote-conf-') as scratch:
        root = Path(scratch).resolve()
        amule_conf = root / 'amule.conf'
        amule_conf.write_text(f'''[ExternalConnect]
ECPort=4712
ECPassword={hashlib.md5(b'regression').hexdigest()}
''')

        remote = run(binary, root, f'--create-config-from={amule_conf}')
        assert mode(remote) == 0o600, oct(mode(remote))

        # An existing file keeps its mode when rewritten, so a loose one must be tightened.
        remote.chmod(0o644)
        run(binary, root, '-w', '-h', '127.0.0.1', '-p', '4712', '-P', 'regression')
        assert mode(remote) == 0o600, oct(mode(remote))
    print('remote.conf permission checks passed')


if __name__ == '__main__':
    main()
