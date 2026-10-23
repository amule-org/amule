#!/usr/bin/env bash
# Run as root on the Ubuntu CI runner. Keep runner/third-party sources intact.
set -euo pipefail

if (( $# == 0 )); then
  echo 'Usage: install-ubuntu-deps.sh PACKAGE...' >&2
  exit 2
fi

. /etc/os-release
if [[ $ID != ubuntu ]]; then
  echo 'This installer requires Ubuntu.' >&2
  exit 2
fi

apt_dir=$(mktemp -d)
trap 'rm -rf "$apt_dir"' EXIT
chmod 755 "$apt_dir"
apt_options=(
  -o "Dir::Etc::sourcelist=$apt_dir/ubuntu.sources"
  -o Dir::Etc::sourceparts=-
  -o Acquire::Retries=2
  -o Acquire::http::Timeout=30
  -o Acquire::https::Timeout=30
)

# A connection timeout does not catch a mirror that continually sends a few
# kB/s. Bound the entire acquisition, then retry on a different official mirror.
# Fetch everything before invoking dpkg, so timeout never interrupts unpacking.
for mirror in archive.ubuntu.com azure.archive.ubuntu.com; do
  cat > "$apt_dir/ubuntu.sources" <<EOF
Types: deb
URIs: https://$mirror/ubuntu/
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates $VERSION_CODENAME-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
  echo "Acquiring Ubuntu dependencies from https://$mirror/ubuntu/"
  if timeout --signal=INT --kill-after=10s 90s \
      apt-get "${apt_options[@]}" update --error-on=any &&
    timeout --signal=INT --kill-after=10s 240s \
      apt-get "${apt_options[@]}" install -y --no-install-recommends --download-only "$@"; then
    # No network transfers remain; failure here must not retry a dpkg operation.
    apt-get "${apt_options[@]}" install -y --no-install-recommends --no-download "$@"
    exit 0
  fi
  echo "::warning::Dependency acquisition failed or exceeded its budget on $mirror."
done

echo '::error::Ubuntu dependency acquisition failed on both official mirrors.' >&2
exit 1
