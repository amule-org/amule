#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""amulegui must leave preferences.dat alone.

amulegui shares its config dir with a local aMule. It used to write the remote core's user
hash into that file, so the local aMule took the remote identity with its own key, and
peers failed its secure identification.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from AllSearchIntegrationTest import free_ports, retry_port_collisions
from DaemonConfig import EC_PASSWORD, OFFLINE, write_amule_conf
from LogfilePathIntegrationTest import daemon_env
from PathDefaultsIntegrationTest import daemon

# Version byte, then a user hash the core will not generate.
LOCAL_PREFS = bytes([0x14]) + bytes(range(0x10, 0x20))


def run(gui, amuled, root):
    core = root / 'core'
    local = root / 'local'
    core.mkdir()
    local.mkdir()
    ports = free_ports(2, peer_index=1)
    write_amule_conf(core, ports[0], Port=ports[1], UDPEnable=0)
    (local / 'preferences.dat').write_bytes(LOCAL_PREFS)
    # The wait below matches an English log line.
    (local / 'remote.conf').write_text(f'''[EC]
Host=127.0.0.1
Port={ports[0]}
Password={hashlib.md5(EC_PASSWORD.encode()).hexdigest()}
[eMule]
Language={OFFLINE['Language']}
''')

    with daemon(amuled, core, ports):
        console_file = root / 'gui-console.log'
        with console_file.open('w') as console:
            gui_proc = subprocess.Popen([gui, '-c', str(local), '-s', '-o'], env=daemon_env(root),
                                        stdout=console, stderr=subprocess.STDOUT)
            try:
                # The core's preferences are applied before the window is up.
                deadline = time.monotonic() + 30
                while True:
                    text = console_file.read_text(errors='replace')
                    if 'Ready' in text:
                        break
                    assert gui_proc.poll() is None, text
                    assert time.monotonic() < deadline, text
                    time.sleep(0.05)
            finally:
                if gui_proc.poll() is None:
                    gui_proc.terminate()
                    gui_proc.wait(timeout=10)

    core_hash = (core / 'preferences.dat').read_bytes()[1:]
    assert core_hash != LOCAL_PREFS[1:], 'fixture hash collided with the core'
    local_prefs = (local / 'preferences.dat').read_bytes()
    assert local_prefs == LOCAL_PREFS, f'preferences.dat rewritten: {local_prefs.hex()}'


def main():
    if sys.platform.startswith('linux') and not (
            os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        print('SKIP: amulegui needs a display (use xvfb-run).')
        return 77
    gui, amuled = (str(Path(arg).resolve()) for arg in sys.argv[1:3])

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-remote-gui-hash-') as temporary:
            run(gui, amuled, Path(temporary).resolve())

    retry_port_collisions(attempt)
    print('amulegui left the local preferences.dat untouched')
    return 0


if __name__ == '__main__':
    sys.exit(main())
