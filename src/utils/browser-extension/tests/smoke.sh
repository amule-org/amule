#!/bin/sh
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

set -eu
TASK_TESTS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Preflight before starting an X server, desktop services or browser.
for tool in node python3 xvfb-run xauth dbus-run-session update-desktop-database; do
    command -v "$tool" >/dev/null || { echo "Missing smoke-test prerequisite: $tool" >&2; exit 77; }
done
python3 -c 'import gi; gi.require_version("Atspi", "2.0"); from gi.repository import Atspi' || exit 77
if [ -n "${AMULE_BINARY:-}" ]; then
    AMULE_BINARY=$(command -v "$AMULE_BINARY") || exit 77
    export AMULE_BINARY
    python3 -c 'import cryptography' || exit 77
fi
node --input-type=module -e 'import {chromium} from "playwright-core"; import fs from "node:fs"; fs.accessSync(process.env.AMULE_BROWSER_EXECUTABLE || chromium.executablePath(), fs.constants.X_OK)' || exit 77
python3 "$TASK_TESTS/../tools/package.py"
TASK_RUN=$(mktemp -d "${TMPDIR:-/tmp}/amule-ed2k-smoke.XXXXXX")
export ED2K_TEST_STATE="$TASK_RUN/state"
echo "Smoke evidence: $TASK_RUN"
python3 "$TASK_TESTS/setup.py"
update-desktop-database "$ED2K_TEST_STATE/data/applications"
# An existing desktop portal can ignore a child's XDG directories. Isolate the
# session bus as well as the display and browser profile before clicking a URI.
# A bare runner has no desktop setting enabling Chromium's native ATK bridge.
# --force-renderer-accessibility alone only establishes renderer accessibility.
# See ui/accessibility/platform/atk_util_auralinux.cc in Chromium.
exec env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u http_proxy -u https_proxy -u all_proxy -u LD_PRELOAD \
    XDG_CONFIG_HOME="$ED2K_TEST_STATE/config" XDG_DATA_HOME="$ED2K_TEST_STATE/data" \
    PATH="$ED2K_TEST_STATE/bin:$PATH" ED2K_PRIVATE_TEST_BUS=1 ACCESSIBILITY_ENABLED=1 xvfb-run -a -s '-screen 0 1280x900x24' \
    dbus-run-session -- node "$TASK_TESTS/smoke.mjs"
