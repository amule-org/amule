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
import ctypes
import time
import gi

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib

def click_native_button(node):
    # A successful AT-SPI DoAction can acknowledge dispatch without activating
    # the native dialog. Use actual X input at the discovered button's center.
    rect = node.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
    assert rect.width > 0 and rect.height > 0 and rect.x >= 0 and rect.y >= 0, rect
    x11 = ctypes.CDLL('libX11.so.6')
    xtst = ctypes.CDLL('libXtst.so.6')
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
    xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    display = x11.XOpenDisplay(None)
    assert display, 'Cannot open the isolated X display'
    try:
        x = rect.x + rect.width // 2
        y = rect.y + rect.height // 2
        assert xtst.XTestFakeMotionEvent(display, -1, x, y, 0)
        assert xtst.XTestFakeButtonEvent(display, 1, 1, 0)
        assert xtst.XTestFakeButtonEvent(display, 1, 0, 0)
        x11.XSync(display, False)
    finally:
        x11.XCloseDisplay(display)


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
                click_native_button(node)
                print('Clicked native Chrome button for ' + origin + ': ' + name)
                sys.exit(0)
            pending.extend(node.get_child_at_index(i) for i in range(node.get_child_count()))
        except GLib.Error:
            continue
    time.sleep(0.1)
print('Native external-app Open button for the test origin not found. Seen: ' + repr(sorted(seen)), file=sys.stderr)
sys.exit(1)
