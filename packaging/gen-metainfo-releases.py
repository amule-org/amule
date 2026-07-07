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
"""Generate the AppStream <releases> block from docs/CHANGELOG.md.

The metainfo template (org.amule.aMule.metainfo.xml.in) carries a single
`@RELEASES@` marker; this script replaces it with one <release> entry per
CHANGELOG version that has a prose summary paragraph (the 3.0.0+ format).
Older contributor-list entries (2.3.3 and earlier) have no summary and are
skipped. Keeping the changelog as the single source of truth stops the
store-visible version from drifting from the shipped one.

No third-party dependencies -- runs anywhere python3 is available: dev
machines, CI, the GNOME SDK build sandbox, and Flathub-at-tag builds.
"""

import argparse
import re
import sys
from html import escape

# "## Version 3.0.1 — "bugfixes and polish"" -> "3.0.1"
_HEADER = re.compile(r"^## Version\s+(\S+)\s+—")
_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _strip_markdown(text):
    # Inline code `x` -> x and bold **x** -> x; metainfo <p> is plain text.
    return text.replace("`", "").replace("**", "")


def parse_changelog(path):
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")

    releases = []
    i, n = 0, len(lines)
    while i < n:
        header = _HEADER.match(lines[i])
        if not header:
            i += 1
            continue
        version = header.group(1)
        i += 1

        # Date is the next non-empty line.
        while i < n and not lines[i].strip():
            i += 1
        if i >= n or not _DATE.match(lines[i].strip()):
            continue
        date = lines[i].strip()
        i += 1

        # The summary is the next paragraph (up to a blank line).
        while i < n and not lines[i].strip():
            i += 1
        para = []
        while i < n and lines[i].strip():
            para.append(lines[i])
            i += 1
        if not para:
            continue

        # Old-format entries open with a tabbed contributor name
        # ("\talesnav:") or a list/heading -- no prose summary. Skip them.
        first = para[0]
        if first.startswith("\t") or first.lstrip()[:1] in ("-", "*", "#"):
            continue

        summary = _strip_markdown(" ".join(s.strip() for s in para)).strip()
        releases.append((version, date, summary))

    return releases


def render_releases(releases):
    out = ["  <releases>"]
    for version, date, summary in releases:
        url = "https://github.com/amule-org/amule/releases/tag/" + version
        out += [
            '    <release version="{0}" date="{1}">'.format(version, date),
            "      <url>{0}</url>".format(url),
            "      <description>",
            "        <p>",
            "          " + escape(summary, quote=False),
            "        </p>",
            "      </description>",
            "    </release>",
        ]
    out.append("  </releases>")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--changelog", required=True)
    parser.add_argument("--template", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    releases = parse_changelog(args.changelog)
    if not releases:
        sys.exit("gen-metainfo-releases: no prose releases found in " + args.changelog)

    with open(args.template, encoding="utf-8") as handle:
        template = handle.read()
    if "@RELEASES@" not in template:
        sys.exit("gen-metainfo-releases: @RELEASES@ marker missing in " + args.template)

    result = template.replace("@RELEASES@", render_releases(releases))
    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(result)


if __name__ == "__main__":
    main()
