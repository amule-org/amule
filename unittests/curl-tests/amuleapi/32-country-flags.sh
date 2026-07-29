#!/usr/bin/env bash
#
# amuleapi 32-country-flags — GET /flags/{code}.png.
#
# The peer / server `country_code` on /clients and /servers is only half
# the story; this route is where a frontend gets the matching artwork.
# The bytes come from the icon table compiled into the binary (the same
# famfamfam set the desktop GUI draws), so the assertions here are about
# the route's shape rather than any file on disk:
#
#   * a known code returns a PNG with the right Content-Type,
#   * the response is cacheable — ETag + Cache-Control — and honours
#     If-None-Match with a 304,
#   * HEAD returns the headers with no body,
#   * the artwork is served verbatim, NOT gzip-encoded (it is already
#     entropy-coded; deflate over a PNG is pure waste),
#   * the "unknown" placeholder (the "??" flag the desktop falls back
#     to for an unresolved code) is reachable under the same route,
#   * anything else that is not exactly two lowercase ASCII letters +
#     .png is a 404, including uppercase, wrong length, traversal
#     attempts and a well-formed code the set has no artwork for,
#   * non-safe methods are 405,
#   * it works with `[Server]/StaticRoot` unset — nothing here reads
#     the file system.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   ./32-country-flags.sh
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_32_country_flags_body.XXXXXX)
CURL_HEAD_FILE=$(mktemp -t amuleapi_32_country_flags_head.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

# Binary-safe: the body lands in a file and stays there. $CURL_BODY is
# deliberately NOT populated (a PNG through a shell variable loses the
# NUL bytes); assertions read $CURL_BODY_FILE instead.
#
# Body *length* comes from curl's own %{size_download}, never from the
# file: curl leaves the previous response's file untouched when a reply
# carries no body (304), and `-I` writes the response headers into it,
# so file size answers a different question than "how many body bytes
# came back". The file is truncated up front anyway so a signature
# check can't read stale bytes.
_curl() {
	local resp
	: > "$CURL_BODY_FILE"
	resp=$(curl -s --max-time 10 \
		-D "$CURL_HEAD_FILE" \
		-o "$CURL_BODY_FILE" -w '%{http_code} %{size_download}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=${resp%% *}
	CURL_SIZE=${resp##* }
	CURL_HEAD=$(cat "$CURL_HEAD_FILE")
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS"
	fi
}

_header() { echo "$CURL_HEAD" | awk -F': ' -v k="$1" 'tolower($1) == k {gsub(/\r/,""); print $2}'; }

if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 32-country-flags @ $HOST"

# --- 1. A known code returns a PNG. -------------------------------
_curl "$HOST/flags/de.png"
_assert_status 200 "GET /flags/de.png"

CT=$(_header content-type)
case "$CT" in
	image/png*) _pass "Content-Type is image/png" ;;
	*) _fail "/flags/de.png Content-Type" "expected image/png, got: ${CT:-<none>}" ;;
esac

# The famfamfam flags are 16x11 8-bit colormap PNGs, a few hundred bytes
# each. Check the 8-byte PNG signature so a future refactor that returns
# the wrong table entry (or an empty body) fails loudly here.
SIG=$(od -An -tx1 -N8 "$CURL_BODY_FILE" | tr -d ' \n')
if [ "$SIG" = "89504e470d0a1a0a" ]; then
	_pass "body starts with the PNG signature"
else
	_fail "/flags/de.png body" "expected PNG magic 89504e470d0a1a0a, got ${SIG:-<empty>}"
fi

if [ "$CURL_SIZE" -gt 100 ] && [ "$CURL_SIZE" -lt 8192 ]; then
	_pass "body is a plausible flag size ($CURL_SIZE bytes)"
else
	_fail "/flags/de.png size" "expected 100..8192 bytes, got $CURL_SIZE"
fi

# --- 2. Cacheable: ETag + Cache-Control. --------------------------
ETAG=$(_header etag)
if [ -n "$ETAG" ]; then
	_pass "GET /flags/de.png emits an ETag ($ETAG)"
else
	_fail "/flags/de.png ETag" "no ETag header found"
fi

CC=$(_header cache-control)
case "$CC" in
	*max-age=*) _pass "Cache-Control carries a max-age ($CC)" ;;
	*) _fail "/flags/de.png Cache-Control" "expected a max-age directive, got: ${CC:-<none>}" ;;
esac

# --- 3. If-None-Match → 304. --------------------------------------
_curl -H "If-None-Match: $ETAG" "$HOST/flags/de.png"
_assert_status 304 "If-None-Match matching ETag → 304"
if [ "$CURL_SIZE" = "0" ]; then
	_pass "304 carries no body"
else
	_fail "304 body" "expected 0 body bytes, got $CURL_SIZE"
fi

# --- 4. HEAD returns headers, no body. ----------------------------
_curl -I "$HOST/flags/de.png"
_assert_status 200 "HEAD /flags/de.png"
if [ "$CURL_SIZE" = "0" ]; then
	_pass "HEAD carries no body"
else
	_fail "HEAD body" "expected 0 body bytes, got $CURL_SIZE"
fi
# Note: like every other amuleapi endpoint, HEAD comes back with
# `Content-Length: 0` rather than the length the GET would return —
# Dispatch() strips the body before Beast sizes the payload. That is
# daemon-wide existing behaviour, not specific to this route, so it is
# deliberately not asserted here.

# --- 5. Already-compressed payload is not gzip-encoded. -----------
# curl offers gzip in Accept-Encoding here. deflate over a PNG buys
# nothing and routinely grows the body, so the server must skip it.
_curl --compressed "$HOST/flags/us.png"
_assert_status 200 "GET /flags/us.png (Accept-Encoding: gzip)"
ENC=$(_header content-encoding)
if [ -z "$ENC" ]; then
	_pass "PNG is served unencoded even when gzip is offered"
else
	_fail "/flags/us.png Content-Encoding" "expected none, got: $ENC"
fi

# --- 6. The "unknown" placeholder is reachable. -------------------
# CCountryFlags::GetFlag() falls back to flags/unknown.png ("??") for
# an empty or unrecognised code; the same artwork has to be available
# here or a frontend can't match the desktop.
_curl "$HOST/flags/unknown.png"
_assert_status 200 "GET /flags/unknown.png (the \"??\" placeholder)"
SIG=$(od -An -tx1 -N8 "$CURL_BODY_FILE" | tr -d ' \n')
if [ "$SIG" = "89504e470d0a1a0a" ]; then
	_pass "placeholder body starts with the PNG signature"
else
	_fail "/flags/unknown.png body" "expected PNG magic, got ${SIG:-<empty>}"
fi

# --- 7. Malformed codes are 404, not 500 / not a wrong icon. ------
# The art id is built by concatenating "flag_" with the code, so the
# charset check is load-bearing: without it a crafted code could name a
# non-flag entry in the shared icon table.
while read -r target label; do
	[ -z "$target" ] && continue
	_curl "$HOST$target"
	_assert_status 404 "$label"
done <<-'EOF'
	/flags/DE.png	uppercase code → 404
	/flags/d.png	one-letter code → 404
	/flags/deu.png	three-letter code → 404
	/flags/de.jpg	wrong extension → 404
	/flags/de	no extension → 404
	/flags/	empty code → 404
	/flags/d1.png	digit in code → 404
	/flags/zz.png	well-formed code with no artwork → 404
EOF

# Traversal attempts. `..` is rejected upstream by the shared
# LooksMalicious gate (400), and the exact-shape check would 404 them
# anyway — either way, no icon comes back.
for target in "/flags/../amule.png" "/flags/%2e%2e/amule.png" "/flags/amule.png"; do
	_curl "$HOST$target"
	case "$CURL_STATUS" in
		400|404) _pass "traversal $target is refused (HTTP $CURL_STATUS)" ;;
		*) _fail "traversal $target" "expected 400 or 404, got $CURL_STATUS" ;;
	esac
	SIG=$(od -An -tx1 -N8 "$CURL_BODY_FILE" | tr -d ' \n')
	if [ "$SIG" = "89504e470d0a1a0a" ]; then
		_fail "traversal $target body" "response leaked a PNG from the icon table"
	fi
done

# --- 8. Non-safe methods are 405. ---------------------------------
for method in POST PATCH DELETE; do
	_curl -X "$method" "$HOST/flags/de.png"
	_assert_status 405 "$method /flags/de.png → 405"
done

# --- 9. No auth required. -----------------------------------------
# Every assertion above ran without a token, so reaching this line at
# all proves it. Assert explicitly with a deliberately bogus bearer so
# a future auth gate on the route can't slip through unnoticed.
_curl -H "Authorization: Bearer not-a-real-token" "$HOST/flags/fr.png"
_assert_status 200 "GET /flags/fr.png with a bogus bearer is still served"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
