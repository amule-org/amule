#!/bin/bash
# Compressed .dmg from a staged folder, with a retry.
#
# Usage:
#   packaging/macos/create-dmg.sh <volume-name> <src-folder> <out.dmg>
#
# hdiutil attaches the image it is building, so it loses to whatever else on
# the machine touches the new volume first (Spotlight, disk arbitration) and
# fails with "hdiutil: create failed - Resource busy". Nothing the caller does
# causes it and nothing prevents it, so retry instead of losing the build:
# observed on CI twice in one morning, a different arch each time. Shared by
# build.sh and the Universal2 job so both get the same behaviour.

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <volume-name> <src-folder> <out.dmg>" >&2
    exit 2
fi

volname="$1"
srcfolder="$2"
out="$3"

for attempt in 1 2 3; do
    rm -f "${out}"
    if hdiutil create \
        -volname "${volname}" \
        -srcfolder "${srcfolder}" \
        -ov -format UDZO \
        "${out}"; then
        exit 0
    fi
    if [[ ${attempt} -eq 3 ]]; then
        echo "fatal: hdiutil create failed ${attempt} times." >&2
        exit 1
    fi
    echo "==> hdiutil create failed (attempt ${attempt}/3), retrying in 15s"
    sleep 15
done
