#!/usr/bin/env python3
#
# This file is part of the aMule Project.
#
# Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
#
"""The Homebrew kegs this machine built from source, read from
`brew info --json=v2 --installed` on stdin.

    built                 names of every source-built formula
    linkable              those brew links into its prefix (not keg-only)
    paths PREFIX NAME...  Cellar and opt paths, relative to /, of the
                          source-built formulae among NAME...

Homebrew records how each keg arrived in its install receipt, so the packaging
workflow caches what this reports instead of a hand-kept list.
"""

import json
import sys


def main():
    mode = sys.argv[1]
    formulae = [
        f
        for f in json.load(sys.stdin)["formulae"]
        if any(i.get("poured_from_bottle") is False for i in f.get("installed", []))
    ]
    if mode == "built":
        names = [f["name"] for f in formulae]
    elif mode == "linkable":
        # Keg-only formulae stay out of the prefix; readline would shadow libedit.
        names = [f["name"] for f in formulae if not f["keg_only"]]
    elif mode == "paths":
        prefix, wanted = sys.argv[2], set(sys.argv[3:])
        for f in formulae:
            if f["name"] not in wanted:
                continue
            print("%s/Cellar/%s" % (prefix, f["name"]))
            # Aliases and old names get an opt link too, and the build shims go
            # through those: pkg-config, never pkgconf.
            for n in [f["name"], *f.get("aliases", []), *f.get("oldnames", [])]:
                print("%s/opt/%s" % (prefix, n))
        return
    else:
        sys.exit("unknown mode: " + mode)
    print(" ".join(sorted(names)))


if __name__ == "__main__":
    main()
