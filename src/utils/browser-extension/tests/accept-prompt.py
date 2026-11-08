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

"""Accept only Chrome's first-use prompt for the isolated test handler."""
import sys
import time
import gi

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib

Atspi.set_timeout(1000, 3000)
origin = sys.argv[1]
seen = set()
end = time.monotonic() + 12
while time.monotonic() < end:
    pending = [Atspi.get_desktop(0)]
    while pending:
        node = pending.pop()
        if node is None:
            continue
        try:
            name = node.get_name() or ''
            if name:
                seen.add(name)
            if (node.get_role() == Atspi.Role.PUSH_BUTTON
                    and name in {'Open aMule Link Test', 'Open xdg-open'}
                    and any(origin in label for label in seen)):
                if not node.get_action_iface().do_action(0):
                    raise RuntimeError('Native prompt button rejected activation')
                print('Accepted native Chrome button for ' + origin + ': ' + name)
                sys.exit(0)
            pending.extend(node.get_child_at_index(i) for i in range(node.get_child_count()))
        except GLib.Error:
            continue
    time.sleep(0.1)
print('Native external-app Open button for the test origin not found. Seen: ' + repr(sorted(seen)), file=sys.stderr)
sys.exit(1)
