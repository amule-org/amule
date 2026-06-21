#!/usr/bin/env bash
#
# amuleapi 27-static-frontend — static-frontend fallthrough.
#
# Asserts that GET / HEAD requests outside `/api/` are served from the
# directory pointed to by `[Server]/StaticRoot` when set; that the
# realpath-based symlink containment rejects escapes; that the 16 MiB
# size cap fires; that conditional GET via `If-None-Match` short-
# circuits to 304; and that the SPA fallback to `index.html` only
# kicks in for extension-less misses (not for missing assets).
#
# Requires `[Server]/StaticRoot` set to a writable directory containing
# an `index.html` (the script plants symlinks + a 17 MiB sentinel there
# during the run). `run-all.sh` provisions /tmp/amuleapi-27-static-frontend-static
# automatically; manual runs need to edit the conf themselves.
# Note: discovery of the install-path default (`AMULEAPI_STATIC_DIR`,
# bundle Resources, wxStandardPaths) is covered by StaticFsTest at the
# unit level; this script covers the static-serve wire mechanics.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   ./27-static-frontend.sh
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_27_static_frontend_body.XXXXXX)
CURL_HEAD_FILE=$(mktemp -t amuleapi_27_static_frontend_head.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	local resp
	resp=$(curl -s --max-time 10 \
		-D "$CURL_HEAD_FILE" \
		-o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
	CURL_HEAD=$(cat "$CURL_HEAD_FILE")
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required for JSON assertions. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

# Resolve the configured StaticRoot from the daemon's conf so the
# script can plant a symlink + oversized file in it during the run.
# run-all.sh provisions a writable /tmp scratch dir before each
# 27-static-frontend run; for manual runs, set StaticRoot in amuleapi.conf to a
# writable directory containing an index.html before launching the
# daemon. (Discovery of the install-path default is unit-tested in
# StaticFsTest and manually verified per-platform — this phase is
# specifically the static-serve mechanics, not the discovery chain.)
CONF_DIR=${AMULEAPI_CONFIG_DIR:-/tmp/amuleapi-regtest}
STATIC_ROOT=$(awk -F= '/^StaticRoot=/ {sub(/[\r ]+$/,"",$2); print $2; exit}' \
	"$CONF_DIR/amuleapi.conf" 2>/dev/null)

if [ -z "$STATIC_ROOT" ]; then
	_die "[Server]/StaticRoot is empty in $CONF_DIR/amuleapi.conf — set it to a writable dir with an index.html, then re-run."
fi

if [ ! -d "$STATIC_ROOT" ]; then
	_die "StaticRoot=$STATIC_ROOT in conf does not exist on disk"
fi
if [ ! -w "$STATIC_ROOT" ]; then
	_die "StaticRoot=$STATIC_ROOT is not writable (27-static-frontend plants symlinks + an oversized file there)"
fi

echo "amuleapi 27-static-frontend @ $HOST — StaticRoot=$STATIC_ROOT"

# Stage transient assets in StaticRoot for the duration of the run.
# Restore the directory's pre-test state on exit so re-running doesn't
# accumulate stale symlinks.
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE" \
	"$STATIC_ROOT/escape.txt" "$STATIC_ROOT/escape.css" \
	"$STATIC_ROOT/huge.bin"' EXIT

# --- 1. GET / serves the placeholder index. -----------------------
_curl "$HOST/"
_assert_status 200 "GET / serves the placeholder index"
case "$CURL_HEAD" in
	*[Cc]ontent-[Tt]ype:*text/html*) _pass "Content-Type is text/html on /" ;;
	*) _fail "/ Content-Type" "expected text/html, got: $(echo "$CURL_HEAD" | grep -i content-type)" ;;
esac

# --- 2. Strong ETag header is present. ----------------------------
ETAG=$(echo "$CURL_HEAD" | awk -F': ' 'tolower($1) == "etag" {gsub(/\r/,""); print $2}')
if [ -n "$ETAG" ]; then
	_pass "GET / emits an ETag header ($ETAG)"
else
	_fail "/ ETag" "no ETag header found in response headers"
fi

# --- 3. If-None-Match → 304. --------------------------------------
_curl -H "If-None-Match: $ETAG" "$HOST/"
_assert_status 304 "If-None-Match matching ETag → 304"

# --- 4. /index.html explicit also works. --------------------------
_curl "$HOST/index.html"
_assert_status 200 "GET /index.html → 200"

# --- 5. SPA fallback for extension-less unknown route. ------------
_curl "$HOST/transfers"
_assert_status 200 "GET /transfers (extension-less unknown) → 200 SPA fallback"

# --- 6. Missing-with-extension is an honest 404. ------------------
_curl "$HOST/missing.css"
_assert_status 404 "GET /missing.css → 404"

# --- 7. /api/v0/* still routes through the API dispatcher. --------
_curl "$HOST/api/v0/version"
_assert_status 200 "GET /api/v0/version is still routed to API (200)"

# --- 8. Symlink-escape containment (extension-bearing). -----------
# `realpath` resolves through the symlink. If the result escapes
# StaticRoot, the daemon must 404. Anything else is a leak.
ln -sf /etc/passwd "$STATIC_ROOT/escape.txt"
_curl "$HOST/escape.txt"
_assert_status 404 "Symlink /escape.txt → /etc/passwd is rejected (404)"
case "$CURL_BODY" in
	*root:*) _fail "escape.txt body" "response leaked /etc/passwd contents" ;;
	*) _pass "/escape.txt response does not leak /etc/passwd" ;;
esac

ln -sf /etc/passwd "$STATIC_ROOT/escape.css"
_curl "$HOST/escape.css"
_assert_status 404 "Symlink /escape.css → /etc/passwd is rejected (404)"

# --- 9. 16 MiB size cap. ------------------------------------------
# Plant a 17 MiB regular file inside StaticRoot. The daemon should
# stat it, see it exceeds the cap, and 404 without reading.
if command -v mkfile >/dev/null 2>&1; then
	mkfile -n 17m "$STATIC_ROOT/huge.bin" >/dev/null
else
	dd if=/dev/zero of="$STATIC_ROOT/huge.bin" bs=1m count=17 \
		status=none 2>/dev/null
fi
_curl "$HOST/huge.bin"
_assert_status 404 "Oversized /huge.bin (>16 MiB) → 404"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
