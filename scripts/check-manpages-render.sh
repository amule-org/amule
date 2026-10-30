#! /bin/bash
#
# Renders every translated manpage the way the build does, so a catalog that
# po4a cannot parse fails here and not in every build that has po4a. The C/C++
# CI skips pull requests that change only docs/man/po, and
# update-manpages-po.sh never renders.
#
# --keep 0 also renders the pages the build leaves out as under-translated.

set -u

cd "$(dirname "$0")/.." || exit 1

if ! command -v po4a > /dev/null; then
	echo "Error: po4a not found. Install po4a (https://po4a.org)." >&2
	exit 20
fi

OUT=$(mktemp -d) || exit 1
trap 'rm -rf "${OUT}"' EXIT

sed \
	-e "s|@CMAKE_CURRENT_SOURCE_DIR@|${PWD}/docs/man|g" \
	-e "s|@CMAKE_CURRENT_BINARY_DIR@|${OUT}/docs/man|g" \
	-e "s|@CMAKE_SOURCE_DIR@|${PWD}|g" \
	-e "s|@CMAKE_BINARY_DIR@|${OUT}|g" \
	docs/man/po4a.config.in > "${OUT}/po4a.config" || exit 1

po4a --no-update --keep 0 "${OUT}/po4a.config"
