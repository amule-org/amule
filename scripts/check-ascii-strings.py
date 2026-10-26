#!/usr/bin/env python3
"""Fails on non-ASCII text in strings that wxWidgets decodes with the locale charset.

_() and plain "..." literals are narrow, so on Windows wx decodes them with the ANSI code page: a
UTF-8 "…" shows as mojibake and, as a msgid, misses every translation. wxT() and L"" are safe.

- Every msgid in po/amule.pot.
- Narrow string literals in the files po/POTFILES.in lists, which are the wx UI sources.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TOKEN = re.compile(r"""//[^\n]*|/\*.*?\*/|'(?:\\.|[^'\\\n])*'|(L|u8|u|U)?"((?:\\.|[^"\\\n])*)\"""", re.S)
WIDE_MACRO = re.compile(r"\b(?:wxT|wxS|_T)\(\s*$")


def pot_failures(pot):
    return [f"{pot.relative_to(ROOT)}:{n}: {line}"
            for n, line in enumerate(pot.read_text(encoding="utf-8").splitlines(), 1)
            if not line.startswith("#") and not line.isascii()]


def source_failures(path):
    text = path.read_text(encoding="utf-8")
    for m in TOKEN.finditer(text):
        if m[2] is None or m[1] in ("L", "u", "U") or m[2].isascii():
            continue
        if WIDE_MACRO.search(text[max(0, m.start() - 16):m.start()]):
            continue
        yield f"{path.relative_to(ROOT)}:{text.count(chr(10), 0, m.start()) + 1}: {m[0]}"


def main():
    failures = pot_failures(ROOT / "po" / "amule.pot")
    listed = (ROOT / "po" / "POTFILES.in").read_text().splitlines()
    for name in listed:
        if name and not name.startswith("#"):
            failures += source_failures(ROOT / name)
    if failures:
        print("Non-ASCII narrow strings (use ASCII, e.g. \"...\" for \"…\", or wxT()):\n  "
              + "\n  ".join(failures))
        return 1
    print(f"All ASCII: po/amule.pot and {len(listed)} sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
