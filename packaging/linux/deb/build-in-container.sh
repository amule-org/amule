#!/bin/bash
# Runs inside the suite's Docker image. build.sh mounts a work dir at /work
# that holds the orig tarballs and the debian/ tree.
#
# Usage: build-in-container.sh <version>

set -euo pipefail

version="$1"
upstream="${version#*:}"
upstream="${upstream%-*}"

export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends dpkg-dev lintian >/dev/null

# shellcheck disable=SC1091
. /etc/os-release
build="/build/amule-${upstream}"
mkdir -p "${build}/wxwidgets"
cp /work/amule_"${upstream}".orig*.tar.* /build/
tar -xzf "/build/amule_${upstream}.orig.tar.gz" -C "${build}" --strip-components=1
# dpkg-source unpacks the wxwidgets component tarball the same way.
tar -xjf "/build/amule_${upstream}.orig-wxwidgets.tar.bz2" -C "${build}/wxwidgets" --strip-components=1
cp -r /work/debian "${build}/debian"

maintainer="$(sed -n 's/^Maintainer: //p' "${build}/debian/control")"
cat > "${build}/debian/changelog" <<EOF
amule (${version}) ${VERSION_CODENAME}; urgency=medium

  * Build for ${NAME} ${VERSION_ID}.

 -- ${maintainer}  $(date -R)
EOF

apt-get -qq build-dep -y --no-install-recommends "${build}" >/dev/null

cd "${build}"
dpkg-buildpackage -us -uc -j"$(nproc)"

cd /build
lintian --fail-on error --suppress-tags bad-distribution-in-changes-file ./*.changes

mkdir -p /work/out
cp ./*.deb ./*.dsc ./*.debian.tar.* ./*.changes ./*.buildinfo ./*.orig*.tar.* /work/out/
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" /work/out
