#!/usr/bin/env bash
#
# amuleapi 13-downloads-delete-clear — clear / delete downloads.
#
# Endpoints:
#   DELETE /api/v0/downloads/{hash}             — drop a single entry
#   POST   /api/v0/downloads/clear_completed    — drop all completed
#
# Routing by current state:
#   * status == "completed" → EC_OP_CLEAR_COMPLETED (by ECID; targets
#                            amuled's m_completedDownloads list)
#   * any other status      → EC_OP_PARTFILE_DELETE (by hash; targets
#                            active partfiles in m_filelist)
#
# The Phase 4h status-decode fix is load-bearing here: a finished
# partfile that the prior decoder reported as "paused" would never
# be enumerable by clear_completed. Phase 5b's bulk endpoint walks
# the cache for `status=="completed"` entries, so the decoder must
# surface the wire string correctly.
#
# Same no-stale-cache invariant as 5a: DELETE response followed by
# an IMMEDIATE GET must show the entry gone (404).

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_13_downloads_delete_clear_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_skip() { TEST_COUNT=$((TEST_COUNT+1)); echo "  SKIP  $1"; }
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

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 13-downloads-delete-clear smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X DELETE "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 401 "DELETE /downloads/{hash} (no token) → 401"

_curl -X POST "$HOST/api/v0/downloads/clear_completed"
_assert_status 401 "POST /downloads/clear_completed (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X DELETE -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/downloads/$TEST_HASH"
	_assert_status 403 "DELETE /downloads/{hash} (guest) → 403"
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/downloads/clear_completed"
	_assert_status 403 "POST /downloads/clear_completed (guest) → 403"
else
	echo "    info: no guest pass; admin-gate skipped"
fi

# --- 2. DELETE non-existent hash → 404. ----------------------------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "DELETE /downloads/{nonexistent} → 404"

# --- 3. Bulk clear_completed on a clean queue (no completed). ------
#
# Pre-seed: clear anything that's already completed so we measure
# from a known baseline. (The smoke is order-independent — 11-downloads-default-filter
# may have left a completed entry behind, or it may not have. We
# call clear once to baseline, then move on.)
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/clear_completed"
_assert_status 200 "POST /downloads/clear_completed (baseline) → 200"
_assert_json_eq '.ok' true 'clear_completed baseline response.ok==true'

# Second call: now nothing is completed. cleared must be 0.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/clear_completed"
_assert_status 200 "POST /downloads/clear_completed (idempotent no-op) → 200"
_assert_json_eq '.cleared' 0 'clear_completed second call cleared 0 entries'

# --- 4. DELETE on an active partfile (happy path + no-stale GET). --
#
# Add the Ubuntu ISO, wait for it to surface, DELETE it, immediate
# GET must 404. This pins the mutate-then-refresh invariant on the
# active-path EC_OP_PARTFILE_DELETE branch.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads"
_assert_status 202 "POST /downloads (Ubuntu ISO) → 202 (setup)"

APPEARED=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads?include_completed=1"
	if printf '%s' "$CURL_BODY" \
	   | jq -e --arg h "$TEST_HASH" '.downloads[] | select(.hash == $h)' \
	   >/dev/null 2>&1; then
		APPEARED=1
		break
	fi
	sleep 0.2
done
[ "$APPEARED" = "1" ] || _die "Ubuntu ISO never surfaced after POST"
_pass "Ubuntu ISO surfaced for DELETE setup"

# DELETE it.
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "DELETE /downloads/{Ubuntu ISO hash} → 200"
_assert_json_eq '.ok'   true        'DELETE response.ok==true'
_assert_json_eq '.hash' "$TEST_HASH" 'DELETE response echoes hash'

# Immediate GET — no stale cache.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 404 "IMMEDIATE GET after DELETE → 404 (no stale cache)"

# Same in list view — entry must be gone from the default response.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads?include_completed=1"
STILL_THERE=$(printf '%s' "$CURL_BODY" \
	| jq --arg h "$TEST_HASH" '[.downloads[] | select(.hash == $h)] | length')
if [ "$STILL_THERE" = "0" ]; then
	_pass "/downloads (list) no longer contains the deleted hash"
else
	_fail "/downloads list staleness" \
		"deleted entry still appears in list response"
fi

# --- 5. DELETE on the same hash twice → 404 second time. ----------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 404 "DELETE the same hash twice → 404 second time"

# --- 6. POST clear_completed {hash:...} on non-existent → 404. ----
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"hash":"baadbaadbaadbaadbaadbaadbaadbaad"}' \
	"$HOST/api/v0/downloads/clear_completed"
_assert_status 404 "POST clear_completed {hash:unknown} → 404"
_assert_json_eq '.error.code' not_found 'clear_completed {hash:unknown}.error.code'

# --- 7. POST clear_completed malformed body → 400. ----------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json at all' \
	"$HOST/api/v0/downloads/clear_completed"
_assert_status 400 "POST clear_completed (malformed body) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"hash": 12345}' \
	"$HOST/api/v0/downloads/clear_completed"
_assert_status 400 "POST clear_completed {hash: non-string} → 400"

# --- 8. POST clear_completed {hash:active-partfile} → 409. --------
#
# Re-add the Ubuntu ISO so we have a known active partfile, then
# call clear_completed with that hash. The handler must refuse with
# 409 not_completed because the entry is in m_filelist, not in
# m_completedDownloads.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads"
_assert_status 202 "POST /downloads (Ubuntu ISO re-add) → 202 (setup)"

APPEARED=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads?include_completed=1"
	if printf '%s' "$CURL_BODY" \
	   | jq -e --arg h "$TEST_HASH" '.downloads[] | select(.hash == $h)' \
	   >/dev/null 2>&1; then
		APPEARED=1
		break
	fi
	sleep 0.2
done
[ "$APPEARED" = "1" ] || _die "Ubuntu ISO never re-surfaced after re-POST"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"hash\":\"$TEST_HASH\"}" \
	"$HOST/api/v0/downloads/clear_completed"
_assert_status 409 "POST clear_completed {hash:active-partfile} → 409"
_assert_json_eq '.error.code' not_completed \
	'clear_completed active hash .error.code == not_completed'

# Cleanup the active partfile.
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "Cleanup: DELETE /downloads/{Ubuntu ISO hash} (2nd) → 200"

# --- 9. DELETE / clear_completed against any naturally-completed
#         entry — covers the by-hash success path AND the 409 from
#         DELETE on a completed entry. SKIP if the daemon has none.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads?include_completed=1"
COMPLETED_HASH=$(printf '%s' "$CURL_BODY" \
	| jq -r '[.downloads[] | select(.status == "completed") | .hash][0] // empty')
if [ -n "$COMPLETED_HASH" ]; then
	# DELETE must refuse with 409 + actionable error code.
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads/$COMPLETED_HASH"
	_assert_status 409 "DELETE /downloads/{completed} → 409"
	_assert_json_eq '.error.code' completed_use_clear_completed \
		'DELETE on completed .error.code == completed_use_clear_completed'

	# clear_completed {hash:completed} → 200, exactly 1 cleared.
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"hash\":\"$COMPLETED_HASH\"}" \
		"$HOST/api/v0/downloads/clear_completed"
	_assert_status 200 "POST clear_completed {hash:completed} → 200"
	_assert_json_eq '.cleared' 1 'per-hash clear_completed cleared exactly 1'
	_assert_json_eq ".cleared_hashes[0]" "$COMPLETED_HASH" \
		'cleared_hashes echoes the input hash'

	# Second call → 404 (entry is gone).
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"hash\":\"$COMPLETED_HASH\"}" \
		"$HOST/api/v0/downloads/clear_completed"
	_assert_status 404 "POST clear_completed {hash:already-cleared} → 404"
else
	_skip "DELETE on completed → 409 (no completed entry in test daemon)"
	_skip "POST clear_completed {hash:completed} → 200 (no completed entry)"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
