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
#      Select a different slice with `?status=active|all|completed`.
#      `include_completed` is gone (a boolean could not express
#      completed-only) and sending it is a 400 naming the
#      replacement.
#      The detail endpoint (`GET /downloads/{hash}`) is UNCHANGED —
#      a consumer asking for a specific file by hash gets it
#      regardless of its status.
#
# Phase 5b exercises the clear-completed mutations:
#   `POST /downloads_clear_completed`              (bulk-clear, no body)
#   `POST /downloads_clear_completed {"hash":...}` (single-entry clear)
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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 11-downloads-default-filter smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
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

# --- 2. ?status=all opt-in. ---------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?status=all"
_assert_status 200 "GET /downloads?status=all → 200"
_assert_json_eq '.downloads | type' array '/downloads?status=all .downloads is array'

ALL_COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
echo "    info: /downloads?status=all returned $ALL_COUNT entries"

# `all` is strictly a superset of the default slice.
if [ "$ALL_COUNT" -ge "$DEFAULT_COUNT" ]; then
	_pass "?status=all returns >= default count ($ALL_COUNT >= $DEFAULT_COUNT)"
else
	_fail "?status=all cardinality" \
		"all count $ALL_COUNT < default count $DEFAULT_COUNT - filter regression"
fi

# If `all` carries at least one entry, its status enum must be from the
# known set (paranoid: the status decoder didn't go off the rails for
# some PS_* code).
if [ "$ALL_COUNT" -gt 0 ]; then
	BOGUS=$(printf '%s' "$CURL_BODY" | jq \
		'[.downloads[].status | select(. != "downloading" and . != "paused" and . != "stopped" and . != "completed" and . != "completing" and . != "hashing" and . != "erroneous" and . != "allocating" and . != "waiting" and . != "insufficient_disk" and . != "unknown")] | length')
	if [ "$BOGUS" = "0" ]; then
		_pass "/downloads?status=all all status values from known enum"
	else
		_fail "/downloads status enum allowlist" \
			"$BOGUS entries have out-of-enum status"
	fi
fi

# --- 3. ?status=active is what the default already does. ----------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?status=active"
_assert_status 200 "GET /downloads?status=active → 200"
ACTIVE_COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
if [ "$ACTIVE_COUNT" = "$DEFAULT_COUNT" ]; then
	_pass "?status=active matches the default slice ($ACTIVE_COUNT entries)"
else
	_fail "?status=active cardinality" \
		"got $ACTIVE_COUNT entries, expected $DEFAULT_COUNT (default)"
fi
_assert_json_eq '[.downloads[].status | select(. == "completed")] | length' 0 \
	'?status=active has zero status=="completed" entries'

# --- 4. ?status=completed - the slice a boolean could not express. --
#
# This is the third state the collection has and the reason
# include_completed was replaced: completed-only was unreachable.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?status=completed"
_assert_status 200 "GET /downloads?status=completed → 200"
_assert_json_eq '.downloads | type' array '/downloads?status=completed .downloads is array'
COMPLETED_COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
echo "    info: /downloads?status=completed returned $COMPLETED_COUNT entries"

# Every row in the completed slice must actually be completed.
_assert_json_eq '[.downloads[].status | select(. != "completed")] | length' 0 \
	'?status=completed carries only status=="completed" entries'

# The three slices partition the queue: active + completed == all.
if [ "$((ACTIVE_COUNT + COMPLETED_COUNT))" = "$ALL_COUNT" ]; then
	_pass "active + completed == all ($ACTIVE_COUNT + $COMPLETED_COUNT == $ALL_COUNT)"
else
	_fail "status slices partition the queue" \
		"active $ACTIVE_COUNT + completed $COMPLETED_COUNT != all $ALL_COUNT"
fi

# --- 4b. Rejections. ----------------------------------------------
#
# A value outside the enum is a 400, not a silent fallthrough to the
# default: a typo must not quietly change what the caller gets.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?status=bogus"
_assert_status 400 "GET /downloads?status=bogus → 400"
_assert_json_eq '.error.code' bad_request 'status=bogus 400 carries error.code=bad_request'

# The old boolean is refused rather than ignored, and the message names
# its replacement so a caller on the old spelling is told where to go.
for v in 1 0 true false; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?include_completed=$v"
	_assert_status 400 "GET /downloads?include_completed=$v → 400"
done
_assert_json_eq '.error.code' bad_request \
	'include_completed 400 carries error.code=bad_request'
if printf '%s' "$CURL_BODY" | grep -q 'status=active|all|completed'; then
	_pass 'include_completed 400 names status=active|all|completed as the replacement'
else
	_fail 'include_completed 400 message' \
		"message does not name the replacement: $(printf '%s' "$CURL_BODY" | head -c 200)"
fi

# --- 5. Detail endpoint UNCHANGED — serves completed files too. ---
#
# Pick a hash from the completed slice. If at least one entry is
# completed, hitting its detail must still return 200 (consumers
# asking for a specific file shouldn't be filtered).
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?status=completed"
COMPLETED_HASH=$(printf '%s' "$CURL_BODY" | jq -r \
	'.downloads | first | .hash // empty')
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
