#!/usr/bin/env bash
#
# amuleapi 20-etag-conditional-get — ETag conditional GET.
#
# Wire contract:
#   * Every GET / HEAD that returns 200 carries an `ETag: "<hex>"`
#     header. The hex is a SHA-256 of the response body truncated to
#     8 bytes (16 hex chars) — short enough to keep header overhead
#     small, with 64 bits of collision resistance.
#   * The server honors `If-None-Match` in four shapes:
#     - `"<hex>"`        — RFC 7232 §2.3 canonical (quoted)
#     - `<hex>`          — bare hex (backward-compat for non-canonical clients)
#     - `W/"<hex>"`      — weak validator
#     - `*`              — wildcard match-any-existing-representation
#     - comma-separated list of any of the above
#   * On a match, the server returns 304 Not Modified with no body but
#     WITH the ETag header preserved (RFC §4.1 — clients use it to
#     re-stamp the cached representation).
#   * Mutations (POST / PATCH / DELETE) are passed through unchanged.
#     If-None-Match on a mutation is ignored — the operation always
#     runs and its response always lands. Phase 5's mutation contract
#     would otherwise silently no-op on retries.
#   * HEAD returns 200 + ETag + empty body (the GET path's body is
#     stripped). HEAD also honors If-None-Match for cache validation.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_20_etag_conditional_get_body.XXXXXX)
CURL_HEAD_FILE=$(mktemp -t amuleapi_20_etag_conditional_get_head.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

# Capture both status + headers. Stores HTTP status in CURL_STATUS,
# response body in CURL_BODY, and the raw header dump in CURL_HEAD.
# Empties both output files before invoking curl — curl leaves the
# -o target untruncated on 304 Not Modified (no body to write), so a
# follow-up read would see whatever the PREVIOUS request wrote.
_curl() {
	: > "$CURL_BODY_FILE"
	: > "$CURL_HEAD_FILE"
	local resp
	resp=$(curl -s --max-time 10 -o "$CURL_BODY_FILE" \
		-D "$CURL_HEAD_FILE" -w '%{http_code}' "$@") \
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

# Extract the ETag header value from the captured response (with
# quotes intact). Returns empty string if absent. Uses sed instead
# of awk because BSD awk (macOS default) doesn't support IGNORECASE.
_get_etag() {
	printf '%s' "$CURL_HEAD" \
		| sed -n 's/^[Ee][Tt][Aa][Gg]:[[:space:]]*\([^[:cntrl:]]*\).*/\1/p' \
		| head -1
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 20-etag-conditional-get smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

sleep 4

# --- 1. GET /version stamps ETag header. ---------------------------
#
# /version is the smallest, most stable response — perfect for ETag
# regression because the digest stays constant across daemon
# restarts (only changes when the build's VERSION macro flips).
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/version"
_assert_status 200 "GET /version → 200"

ETAG=$(_get_etag)
if [ -n "$ETAG" ]; then
	_pass "GET /version carries ETag header (value: $ETAG)"
else
	_fail "GET /version ETag" "ETag header absent"
	_die "cannot continue without an ETag value"
fi

# ETag must be quoted per RFC §2.3.
case "$ETAG" in
	\"*\") _pass "ETag value is quoted (RFC 7232 §2.3)" ;;
	*)     _fail "ETag quoting" "expected \"<hex>\", got $ETAG" ;;
esac

# Extract the bare hex (strip outer quotes).
BARE_HEX=$(echo "$ETAG" | sed 's/^"//; s/"$//')
case "$BARE_HEX" in
	*[!0-9a-f]*) _fail "ETag hex chars" \
	             "non-hex character in payload: $BARE_HEX" ;;
	*) ;;
esac
LEN=${#BARE_HEX}
if [ "$LEN" = "16" ]; then
	_pass "ETag payload is 16 lowercase hex chars (8-byte digest truncation)"
else
	_fail "ETag hex length" "expected 16, got $LEN"
fi

# --- 2. Conditional GET — RFC-canonical (quoted) → 304. ----------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: $ETAG" "$HOST/api/v0/version"
_assert_status 304 "GET /version + If-None-Match (quoted) → 304"

# Body must be empty on 304.
if [ -z "$CURL_BODY" ]; then
	_pass "304 response carries no body"
else
	_fail "304 body" "expected empty, got $(echo "$CURL_BODY" | head -c 80)"
fi

# ETag must be preserved on 304 (RFC §4.1).
ETAG_ON_304=$(_get_etag)
if [ "$ETAG_ON_304" = "$ETAG" ]; then
	_pass "304 response preserves ETag header ($ETAG_ON_304)"
else
	_fail "304 ETag preservation" \
		"expected $ETAG, got $ETAG_ON_304"
fi

# --- 3. Conditional GET — bare hex (backward-compat) → 304. ------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: $BARE_HEX" "$HOST/api/v0/version"
_assert_status 304 "GET /version + If-None-Match (bare hex) → 304"

# --- 4. Conditional GET — weak validator → 304. ------------------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: W/$ETAG" "$HOST/api/v0/version"
_assert_status 304 "GET /version + If-None-Match (W/-prefixed) → 304"

# --- 5. Conditional GET — wildcard → 304. ------------------------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: *" "$HOST/api/v0/version"
_assert_status 304 "GET /version + If-None-Match: * → 304"

# --- 6. Conditional GET — wrong hex → 200 (no match). ------------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H 'If-None-Match: "feedfacefeedface"' "$HOST/api/v0/version"
_assert_status 200 "GET /version + If-None-Match (wrong hex) → 200"

# --- 7. Comma-separated list — any-match wins. -------------------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: \"feedfacefeedface\", $ETAG" \
	"$HOST/api/v0/version"
_assert_status 304 "GET /version + If-None-Match (list, hit in 2nd entry) → 304"

# --- 8. HEAD honors If-None-Match too. ---------------------------
_curl -X HEAD -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/version"
_assert_status 200 "HEAD /version → 200"
HEAD_ETAG=$(_get_etag)
if [ "$HEAD_ETAG" = "$ETAG" ]; then
	_pass "HEAD /version carries the same ETag as GET ($HEAD_ETAG)"
else
	_fail "HEAD ETag parity" \
		"GET ETag=$ETAG, HEAD ETag=$HEAD_ETAG"
fi
# HEAD body always empty.
if [ -z "$CURL_BODY" ]; then
	_pass "HEAD /version carries no body"
else
	_fail "HEAD body" "expected empty, got $(echo "$CURL_BODY" | head -c 80)"
fi

_curl -X HEAD -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: $ETAG" "$HOST/api/v0/version"
_assert_status 304 "HEAD /version + If-None-Match → 304"

# --- 9. ETag is stamped on every safe-method 200 response. -------
#
# Walk a representative subset of GET endpoints — every one must
# carry an ETag header per the Dispatch wrapper contract.
for ep in status downloads shared clients servers kad categories preferences; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/$ep"
	if [ "$CURL_STATUS" != "200" ]; then
		_fail "GET /$ep status" "expected 200, got $CURL_STATUS"
		continue
	fi
	E=$(_get_etag)
	if [ -n "$E" ]; then
		_pass "GET /$ep carries ETag header"
	else
		_fail "GET /$ep ETag" "ETag header absent"
	fi
done

# --- 10. Mutation responses are NEVER 304'd. ---------------------
#
# Even with If-None-Match matching, POST / PATCH / DELETE must run.
# Otherwise Phase 5's mutate-then-refresh contract silently no-ops
# on retries — a wire-contract footgun.
#
# Capture the ETag of /preferences before mutating, then PATCH with
# If-None-Match: matching the GET's ETag. The PATCH must execute
# (returns 200) — not skip with 304.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
PREF_ETAG=$(_get_etag)
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-H "If-None-Match: $PREF_ETAG" \
	-d '{"connection":{"max_upload_kbps":0}}' \
	"$HOST/api/v0/preferences"
if [ "$CURL_STATUS" = "200" ]; then
	_pass "PATCH ignores If-None-Match (status=200, not 304)"
else
	_fail "PATCH /preferences with If-None-Match" \
		"expected 200, got $CURL_STATUS (mutation skipped?)"
fi

# Same for POST on /search.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-H "If-None-Match: $PREF_ETAG" \
	-d '{"query":"ubuntu"}' \
	"$HOST/api/v0/search"
if [ "$CURL_STATUS" = "202" ]; then
	_pass "POST ignores If-None-Match (status=202, not 304)"
else
	_fail "POST /search with If-None-Match" \
		"expected 202, got $CURL_STATUS"
fi

# Stop the search to leave the daemon clean.
curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/stop" > /dev/null

# --- 11. Error responses (4xx/5xx) don't get ETag stamped. -------
#
# Stamping a 4xx body with ETag would lure clients into caching
# error responses — anti-feature. Phase 7's Dispatch wrapper guards
# `resp.status == 200` so 4xx passes through unchanged.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "GET /downloads/{nonexistent} → 404"
ERR_ETAG=$(_get_etag)
if [ -z "$ERR_ETAG" ]; then
	_pass "404 response carries no ETag (errors not cached)"
else
	_fail "404 ETag suppression" \
		"unexpected ETag on 404: $ERR_ETAG"
fi

# --- 12. ETag is stable across ticks when data IS stable. --------
#
# Phase 7.1 retired snapshot_at from the envelope — list endpoints
# now only churn their ETag when actual data churns. /preferences is
# the most reliable "stable" surface for this check (no per-tick
# refresh — it changes only when an operator runs PATCH).
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
P1=$(_get_etag)
sleep 2
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
P2=$(_get_etag)
if [ "$P1" = "$P2" ] && [ -n "$P1" ]; then
	_pass "ETag stable on /preferences across 2 s (no churn → cacheable)"
else
	_fail "ETag stability on /preferences" \
		"E1=$P1, E2=$P2 — Phase 7.1's snapshot_at removal should make this stable"
fi

# A second cache-hit observable: the second request with
# If-None-Match: <P1> against /preferences MUST 304.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "If-None-Match: $P1" "$HOST/api/v0/preferences"
_assert_status 304 "GET /preferences + If-None-Match (cached) → 304 (Phase 7.1 cache works)"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
