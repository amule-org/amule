#!/usr/bin/env python3
"""Fails when two names share a value where values must be unique.

- Window IDs in src/muuli_wdr.h: FindWindow() returns the first match, so a clash makes
  CastChild() return the wrong control.
- Each enum in the EC .abstract files: a repeated opcode or tag code is ambiguous on the wire,
  and only a Debug build catches it.

Two branches can each add the same number in different places and merge with no conflict, so
this runs in CI rather than relying on review.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def clashes(pairs, fmt):
    owner, found = {}, []
    for name, value in pairs:
        if value in owner:
            found.append(f"{value:{fmt}}: {owner[value]} and {name}")
        else:
            owner[value] = name
    return found


def window_ids(path):
    define = re.compile(r"^#define\s+(\w+)\s+(\d+)\s*$")
    return [(m[1], int(m[2])) for m in map(define.match, path.read_text().splitlines()) if m]


def abstract_enums(path):
    """Yields (enum name, [(member, value)]) for every Enum section."""
    member = re.compile(r"^\s*(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$")
    section, name, members = False, None, []
    for line in path.read_text().splitlines():
        if line.startswith("[Section Content]"):
            section, name, members = True, None, []
        elif line.startswith("[/Section]"):
            if section and name:
                yield name, members
            section = False
        elif section and line.startswith("Type "):
            if line.split()[1] != "Enum":
                section = False
        elif section and line.startswith("Name "):
            name = line.split()[1]
        elif section and not line.startswith(("#", "DataType ")):
            m = member.match(line)
            if m:
                members.append((m[1], int(m[2], 0)))


def main():
    failures = []
    header = ROOT / "src" / "muuli_wdr.h"
    ids = window_ids(header)
    failures += [f"{header.relative_to(ROOT)}: {c}" for c in clashes(ids, "d")]
    checked = [f"{len(ids)} window IDs"]
    for abstract in sorted((ROOT / "src" / "libs" / "ec" / "abstracts").glob("*.abstract")):
        for name, members in abstract_enums(abstract):
            failures += [f"{abstract.relative_to(ROOT)} {name}: {c}" for c in clashes(members, "#x")]
            checked.append(f"{len(members)} {name}")
    if failures:
        print("Duplicate values:\n  " + "\n  ".join(failures))
        return 1
    print("All unique: " + ", ".join(checked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
