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
        printf '%s\0%s\0' "${repo}/${distro}/${codename}" "${deb}"
    done
# Each push waits about 9 s for Cloudsmith to process the package.
done | xargs -0 -n 2 -P 8 cloudsmith push deb --republish
