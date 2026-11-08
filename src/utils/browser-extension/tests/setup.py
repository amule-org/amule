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

"""Prepare a private desktop handler and unpack the exact release under test."""
from pathlib import Path
import os
import json
import shutil
import socket
import sys
import zipfile

root = Path(__file__).resolve().parent
state = Path(os.environ['ED2K_TEST_STATE'])
state.mkdir()
with zipfile.ZipFile(root.parent / 'dist/ed2k-link-fixer.zip') as archive:
    assert archive.testzip() is None
    archive.extractall(state / 'release')
for directory in ['bin', 'config', 'data/applications', 'amule/Temp', 'amule/Incoming']:
    (state / directory).mkdir(parents=True, exist_ok=True)


# xdg-open's generic launcher does not parse a quoted Exec command like GIO
# does. Unquoted absolute interpreter/script paths work through both launchers
# when the snapshot is in a private no-space directory, even if the checkout
# contains spaces. Invoke Python explicitly to allow noexec temporary mounts.
# Snapshot the script so later source edits cannot alter an active run.
handler = state / 'bin/amule-ed2k-test-handler'
shutil.copyfile(root / 'handler.py', handler)
handler.chmod(0o755)
(state / 'runtime.json').write_text(json.dumps({'binary': os.environ.get('AMULE_BINARY')}))
(state / 'data/applications/org.amule.aMule.desktop').write_text(
    '[Desktop Entry]\nName=aMule Link Test\nType=Application\n'
    f'Exec={sys.executable} {handler} %u\n'
    'MimeType=x-scheme-handler/ed2k;\n')
(state / 'config/mimeapps.list').write_text(
    '[Default Applications]\nx-scheme-handler/ed2k=org.amule.aMule.desktop\n')
# aMule also binds auxiliary ports relative to its TCP port; zero is not
# an ephemeral-port request for those listeners. Choose an unused range.
for attempt in range(50):
    sockets = []
    try:
        first = socket.socket()
        sockets.append(first)
        first.bind(('127.0.0.1', 0))
        port = first.getsockname()[1]
        for offset in range(1, 4):
            extra = socket.socket()
            sockets.append(extra)
            extra.bind(('127.0.0.1', port + offset))
        break
    except OSError:
        if attempt == 49:
            raise
    finally:
        for item in sockets:
            item.close()

(state / 'amule/amule.conf').write_text(
    '[eMule]\nFirstRunWizardDone=1\nAutoconnect=0\nNewVersionCheck=0\n'
    'GeoIPEnabled=0\nGeoIPAutoUpdate=0\nConnectToKad=0\nConnectToED2K=0\n'
    f'UPnPEnabled=0\nPort={port}\nUDPEnable=0\n'
    f'TempDir={state}/amule/Temp\nIncomingDir={state}/amule/Incoming\n')
# The version notice is independent of AppVersion in amule.conf.
(state / 'amule/lastversion').write_text('GIT\n3.0.1\n3.2.0\n')
if os.environ.get('AMULE_BINARY'):
    assert shutil.which(os.environ['AMULE_BINARY']), 'AMULE_BINARY is not executable'
    # Reuse the project's offline application fixture. In particular amuled
    # exits unless EC is configured, even when only CLI link delivery is used.
    sys.path.insert(0, str(root.parents[3] / 'unittests/tests'))
    from DaemonConfig import write_amule_conf
    write_amule_conf(state / 'amule', port + 1, Port=port, UDPEnable=0,
                     Autoconnect=0, GeoIPAutoUpdate=0)

print(state)
