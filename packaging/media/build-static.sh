#!/bin/sh
# Pinned static MediaInfoLib/ZenLib for every bundle; no curl or libmms.
set -eu
prefix=${1:-/usr/local}
work=${2:-/tmp/amule-media-deps}
if [ "$#" -ge 2 ]; then shift 2; else set --; fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$script_dir/../linux/versions.env"
mkdir -p "$work"
python3 - "$work" "$ZENLIB_TARBALL_URL" "$ZENLIB_SHA256" \
  "$MEDIAINFO_TARBALL_URL" "$MEDIAINFO_SHA256" <<'PYTHON'
import hashlib, pathlib, subprocess, sys, urllib.request
work = pathlib.Path(sys.argv[1])
for name, url, checksum in (('zenlib', *sys.argv[2:4]), ('mediainfo', *sys.argv[4:6])):
    path = work / (name + '.tar.gz')
    if not path.exists(): path.write_bytes(urllib.request.urlopen(url).read())
    if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
        raise SystemExit('Checksum mismatch: ' + name)
    target = work / name
    target.mkdir(exist_ok=True)
    subprocess.run(['tar', '-xf', str(path), '-C', str(target), '--strip-components=1'], check=True)
PYTHON
for project in zenlib mediainfo; do
  cmake -S "$work/$project/Project/CMake" -B "$work/$project-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_ZENLIB=OFF -DBUILD_ZLIB=OFF -DCMAKE_DISABLE_FIND_PACKAGE_TinyXML=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON \
    '-DCMAKE_CXX_FLAGS=-DMEDIAINFO_LIBCURL_NO -DMEDIAINFO_LIBMMS_NO' "$@"
  cmake --build "$work/$project-build" -j2
  cmake --install "$work/$project-build"
done
mkdir -p "$prefix/share/licenses/amule-media-probe"
cp "$script_dir"/licenses/* "$prefix/share/licenses/amule-media-probe/"
