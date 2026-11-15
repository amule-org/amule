#!/bin/bash
# Runs inside the suite's Docker image. build.sh mounts a work dir at /work
# that holds the orig tarballs and the debian/ tree.
#
# Usage: build-in-container.sh <version>
# CROSS_ARCH, when set, cross-builds for that Debian architecture.

set -euo pipefail

version="$1"
upstream="${version#*:}"
upstream="${upstream%-*}"

export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends dpkg-dev lintian >/dev/null
# uutils chmod fails with EOVERFLOW for 32-bit arm on a 64-bit kernel.
if [ "$(dpkg --print-architecture)" = armhf ] && dpkg -s coreutils-from-uutils >/dev/null 2>&1; then
    apt-get -qq install -y --allow-remove-essential coreutils-from-gnu coreutils-from-uutils- >/dev/null
fi

# shellcheck disable=SC1091
. /etc/os-release

cross=()
if [ -n "${CROSS_ARCH:-}" ]; then
    native="$(dpkg --print-architecture)"
    dpkg --add-architecture "${CROSS_ARCH}"
    # Ubuntu keeps amd64 and i386 on the main archive and the other arches on ports.
    if [ "${ID}" = ubuntu ] && { [ "${native}" = amd64 ] || [ "${native}" = i386 ]; }; then
        if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
            sed -i "/^Types:/a Architectures: ${native}" /etc/apt/sources.list.d/ubuntu.sources
        else
            sed -i "s/^deb /deb [arch=${native}] /" /etc/apt/sources.list
        fi
        cat > /etc/apt/sources.list.d/ports.sources <<EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: ${VERSION_CODENAME} ${VERSION_CODENAME}-updates ${VERSION_CODENAME}-security
Components: main universe
Architectures: ${CROSS_ARCH}
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
    fi
    apt-get -qq update
    apt-get -qq install -y --no-install-recommends "crossbuild-essential-${CROSS_ARCH}" >/dev/null
    cross=(-a "${CROSS_ARCH}")
fi
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

# A security update can reach one arch before the other, and a Multi-Arch:
# same package must have one version on both. Pin each such package to the
# newest version both arches have.
if [ -n "${CROSS_ARCH:-}" ]; then
    for _ in 1 2 3; do
        skewed="$(apt-get build-dep -s --no-install-recommends "${cross[@]}" "${build}" 2>&1 |
            sed -n "s/.* : Depends: \([^ :]*\):${CROSS_ARCH}.*/\1/p" | sort -u)" || true
        pinned=0
        for pkg in ${skewed}; do
            grep -qs "^Package: ${pkg} " /etc/apt/preferences.d/arch-skew && continue
            apt-cache show "${pkg}:${CROSS_ARCH}" 2>/dev/null |
                awk '/^Multi-Arch: same/ { same = 1 } END { exit !same }' || continue
            common="$(comm -12 \
                <(apt-cache madison "${pkg}:${native}" | awk '{print $3}' | sort -u) \
                <(apt-cache madison "${pkg}:${CROSS_ARCH}" | awk '{print $3}' | sort -u) |
                sort -V | tail -n1)"
            [ -n "${common}" ] || continue
            echo "Pinning ${pkg} to ${common} for both arches"
            printf 'Package: %s %s:%s\nPin: version %s\nPin-Priority: 1001\n\n' \
                "${pkg}" "${pkg}" "${CROSS_ARCH}" "${common}" >> /etc/apt/preferences.d/arch-skew
            pinned=1
        done
        [ "${pinned}" = 1 ] || break
    done
fi

apt-get -qq build-dep -y --no-install-recommends --allow-downgrades ${cross[@]+"${cross[@]}"} "${build}" >/dev/null

cd "${build}"
dpkg-buildpackage -us -uc -j"$(nproc)" ${cross[@]+"${cross[@]}"}

cd /build
lintian --fail-on error --suppress-tags bad-distribution-in-changes-file ./*.changes

mkdir -p /work/out
cp ./*.deb ./*.dsc ./*.debian.tar.* ./*.changes ./*.buildinfo ./*.orig*.tar.* /work/out/
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" /work/out
