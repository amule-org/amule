#!/usr/bin/env bash
#
# Download and refresh the vendored frontend libraries.
#
# The web frontend ships every third-party library locally (no build step, no
# bundler, no npm/CDN at runtime). This script re-downloads the minified ES
# modules into static/js/vendor/, fixes the version comment on the first line
# of each file, and refreshes the matching LICENSE files.
#
# To bump a library: edit the *_VERSION constant below and re-run this script.
#
#   ./tools/update-vendor.sh
#
set -euo pipefail

# --- pinned versions (edit these to upgrade) ---------------------------------
PREACT_VERSION="10.29.2"   # also provides preact/hooks
HTM_VERSION="3.1.1"

# --- paths -------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATIC_DIR="$(cd "${SCRIPT_DIR}/../static" && pwd)"
JS_DIR="${STATIC_DIR}/js/vendor"

mkdir -p "${JS_DIR}"

# fetch_lib <name> <version> <url> <dest-file>
#
# Downloads <url>, prepends a "// <name> vX" comment as the first line, and
# writes it to <dest-file>.
fetch_lib() {
  local name="$1" version="$2" url="$3" dest="$4"
  local tmp
  tmp="$(mktemp)"

  echo "  ${name} v${version}"
  curl -fsSL --retry 3 -o "${tmp}" "${url}"

  # Drop the trailing sourceMappingURL comment: we don't vendor the .map
  # files, so the reference would only 404 in devtools.
  sed -i -E '/^[[:space:]]*\/\/# sourceMappingURL=/d; /^[[:space:]]*\/\*# sourceMappingURL=/d' "${tmp}"

  { printf '// %s v%s\n' "${name}" "${version}"; cat "${tmp}"; } > "${dest}"
  rm -f "${tmp}"
}

# fetch_license <version-pinned package path> <dest-file>
fetch_license() {
  echo "  LICENSE -> $(basename "$2")"
  curl -fsSL --retry 3 -o "$2" "https://unpkg.com/$1/LICENSE"
}

echo "Vendoring frontend libraries into ${JS_DIR}"

# --- preact (+ hooks) --------------------------------------------------------
fetch_lib "preact" "${PREACT_VERSION}" \
  "https://unpkg.com/preact@${PREACT_VERSION}/dist/preact.module.js" \
  "${JS_DIR}/preact.module.js"
fetch_lib "preact/hooks" "${PREACT_VERSION}" \
  "https://unpkg.com/preact@${PREACT_VERSION}/hooks/dist/hooks.module.js" \
  "${JS_DIR}/hooks.module.js"
fetch_license "preact@${PREACT_VERSION}" "${JS_DIR}/preact.LICENSE"

# --- htm ---------------------------------------------------------------------
fetch_lib "htm" "${HTM_VERSION}" \
  "https://unpkg.com/htm@${HTM_VERSION}/dist/htm.module.js" \
  "${JS_DIR}/htm.module.js"
fetch_license "htm@${HTM_VERSION}" "${JS_DIR}/htm.LICENSE"

echo "Done. Vendored:"
echo "  preact ${PREACT_VERSION}, preact/hooks ${PREACT_VERSION}, htm ${HTM_VERSION}"
