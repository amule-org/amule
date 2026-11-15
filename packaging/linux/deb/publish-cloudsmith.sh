#!/bin/bash
# Uploads the .deb files of a deb.yml run to a Cloudsmith repository.
#
# Usage: publish-cloudsmith.sh <workspace/repo> <dir>
# <dir> holds one deb-<suite>-<arch> folder per build job, as
# actions/download-artifact leaves them. Needs CLOUDSMITH_API_KEY.

set -euo pipefail

repo="$1"
dir="$2"

for job in "${dir}"/deb-*/; do
    suite="$(basename "${job}")"
    suite="${suite#deb-}"
    suite="${suite%-*}"
    distro="${suite%%[0-9]*}"
    codename="$(sed -n 's/^Distribution: //p' "${job}"*.changes)"
    for deb in "${job}"*.deb; do
        # Every job builds the Architecture: all packages, but a suite can
        # hold only one copy of each.
        if [[ "${deb}" == *_all.deb && "${job}" != *-amd64/ ]]; then
            continue
        fi
        cloudsmith push deb "${repo}/${distro}/${codename}" "${deb}" --republish
    done
done
