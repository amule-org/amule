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

"""Build the unpacked extension ZIP without bundling tests or local state."""
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parent.parent
output = root / 'dist' / 'ed2k-link-fixer.zip'
files = ['manifest.json', 'rewrite.js', 'popup.html', 'help.html', 'style.css',
         'README.md', 'LICENSE.txt', 'icons/source.svg']
files.extend(f'icons/{size}.png' for size in (16, 32, 48, 128))
output.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
    for name in sorted(files):
        info = zipfile.ZipInfo(f'ed2k-link-fixer/{name}', (2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, (root / name).read_bytes())
print(output)
