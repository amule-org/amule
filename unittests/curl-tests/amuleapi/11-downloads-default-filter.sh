#!/usr/bin/env bash
#
# amuleapi 11-downloads-default-filter — /downloads default-filter completed + status
# decode fix.
#
# Two related changes:
#   1. DownloadStatusName at PS_COMPLETE / PS_COMPLETING short-circuits
#      BEFORE the `stopped` check. amuled holds finished downloads in
#      `m_completedDownloads` with EC_TAG_PARTFILE_STOPPED set true;
#      the old decoder reported "paused" for them, masking the
#      completion state. Fix exposes the "completed" wire string the
#      schema documented.
#   2. /downloads list filters status=="completed" entries by default.
#      `m_completedDownloads` is amuled's own awaiting-clear list;
#      surfacing those alongside the active queue confuses consumers.
#      Opt back in with `?include_completed=1`.
#      The detail endpoint (`GET /downloads/{hash}`) is UNCHANGED —
#      a consumer asking for a specific file by hash gets it
#      regardless of its status.
#
# Phase 5b exercises the clear-completed mutations:
#   `POST /downloads/clear_completed`              (bulk-clear, no body)
#   `POST /downloads/clear_completed {"hash":...}` (single-entry clear)
# Both wire to EC_OP_CLEAR_COMPLETED. `DELETE /downloads/{hash}` is
# active-only and 409s on completed entries — see 13-downloads-delete-clear for the
# 409 + per-entry clear assertions.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_11_downloads_default_filter_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

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
	resp=$(curl -s --max-time 10 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
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

_assert_json_eq() {
	local expr=$1 expected=$2 label=$3
	local actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null) \
		|| _fail "$label" "body was not valid JSON" "body: $CURL_BODY"
	if [ "$actual" = "$expected" ]; then
		_pass "$label"
	else
		_fail "$label" "expected $expected, got $actual" "body: $CURL_BODY"
	fi
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 11-downloads-default-filter smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"
sleep 4

# --- 1. Default /downloads — completed entries filtered out. ------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads"
_assert_status 200 "GET /downloads → 200"
_assert_json_eq '.downloads | type' array '/downloads .downloads is array'

# By definition: no entry in the default response should carry
# status == "completed". This is the contract; smoke pins it
# regardless of the daemon's current download mix.
BOGUS=$(printf '%s' "$CURL_BODY" | jq '[.downloads[].status | select(. == "completed")] | length')
if [ "$BOGUS" = "0" ]; then
	_pass "/downloads default has zero status==\"completed\" entries (filter active)"
else
	_fail "/downloads default filter" \
		"$BOGUS entries with status==completed leaked through the default filter"
fi

DEFAULT_COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
echo "    info: /downloads default returned $DEFAULT_COUNT entries (completed filtered)"

# --- 2. ?include_completed=1 opt-in. ------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?include_completed=1"
_assert_status 200 "GET /downloads?include_completed=1 → 200"
_assert_json_eq '.downloads | type' array '/downloads?include_completed=1 .downloads is array'

OPT_IN_COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
echo "    info: /downloads?include_completed=1 returned $OPT_IN_COUNT entries"

# The opt-in count must be >= the default count (filter is strictly
# additive on the opt-in path).
if [ "$OPT_IN_COUNT" -ge "$DEFAULT_COUNT" ]; then
	_pass "?include_completed=1 returns >= default count ($OPT_IN_COUNT >= $DEFAULT_COUNT)"
else
	_fail "?include_completed=1 cardinality" \
		"opt-in count $OPT_IN_COUNT < default count $DEFAULT_COUNT — filter regression"
fi

# If the opt-in carries at least one entry, its status enum must be
# from the known set (paranoid: the status decoder didn't go off the
# rails for some PS_* code).
if [ "$OPT_IN_COUNT" -gt 0 ]; then
	BOGUS=$(printf '%s' "$CURL_BODY" | jq \
		'[.downloads[].status | select(. != "downloading" and . != "paused" and . != "stopped" and . != "completed" and . != "completing" and . != "hashing" and . != "erroneous" and . != "allocating" and . != "waiting" and . != "insufficient_disk" and . != "unknown")] | length')
	if [ "$BOGUS" = "0" ]; then
		_pass "/downloads?include_completed=1 all status values from known enum"
	else
		_fail "/downloads status enum allowlist" \
			"$BOGUS entries have out-of-enum status"
	fi
fi

# --- 3. Other truthy values: include_completed=true, =yes. --------
for v in true yes; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?include_completed=$v"
	_assert_status 200 "GET /downloads?include_completed=$v → 200"
	c=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
	if [ "$c" = "$OPT_IN_COUNT" ]; then
		_pass "include_completed=$v matches include_completed=1 ($c entries)"
	else
		_fail "include_completed=$v cardinality" \
			"got $c entries, expected $OPT_IN_COUNT"
	fi
done

# --- 4. include_completed=0 / =false / =garbage → default behavior. -
for v in 0 false bogus; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?include_completed=$v"
	_assert_status 200 "GET /downloads?include_completed=$v → 200"
	c=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
	if [ "$c" = "$DEFAULT_COUNT" ]; then
		_pass "include_completed=$v falls through to default ($c entries)"
	else
		_fail "include_completed=$v fallthrough" \
			"got $c entries, expected $DEFAULT_COUNT (default)"
	fi
done

# --- 5. Detail endpoint UNCHANGED — serves completed files too. ---
#
# Pick a hash from the opt-in response. If at least one entry is
# completed, hitting its detail must still return 200 (consumers
# asking for a specific file shouldn't be filtered).
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?include_completed=1"
COMPLETED_HASH=$(printf '%s' "$CURL_BODY" | jq -r \
	'[.downloads[] | select(.status == "completed")] | first | .hash // empty')
if [ -n "$COMPLETED_HASH" ]; then
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$COMPLETED_HASH"
	_assert_status 200 "GET /downloads/{completed-hash} → 200 (detail not filtered)"
	_assert_json_eq '.status' completed \
		'/downloads/{completed-hash} carries status=completed (decoder fix observable)'
	_assert_json_eq '.hash' "$COMPLETED_HASH" \
		'/downloads/{completed-hash} echoes the requested hash'
else
	echo "    info: no completed downloads in this daemon's queue; detail-not-filtered check skipped"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
