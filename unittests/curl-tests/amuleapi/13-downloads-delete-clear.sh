#!/usr/bin/env bash
#
# amuleapi 13-downloads-delete-clear — clear / delete downloads.
#
# Endpoints:
#   DELETE /api/v1/downloads/{hash}             — drop a single entry
#   POST   /api/v1/downloads_clear_completed    — drop all completed
#
# Completed deletion is exercised with disposable fixtures by
# CompletedFileDeleteIntegrationTest.py. This script only clears existing
# completed notifications, preserving their files.
#
# The status decode is load-bearing here: a finished
# partfile that the prior decoder reported as "paused" would never
# be enumerable by clear_completed. The bulk clear-completed endpoint walks
# the cache for `status=="completed"` entries, so the decoder must
# surface the wire string correctly.
#
# Same no-stale-cache invariant as 5a: DELETE response followed by
# an IMMEDIATE GET must show the entry gone (404).

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

_preflight

echo "amuleapi 13-downloads-delete-clear smoke @ $HOST"

ADMIN_TOKEN=$(_login "$ADMIN_PASS")
[ -n "$ADMIN_TOKEN" ] || _die "admin login failed"

GUEST_TOKEN=$(_login "$GUEST_PASS")
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X DELETE "$API/downloads/$TEST_HASH"
_assert_status 401 "DELETE /downloads/{hash} (no token) → 401"

_curl -X POST "$API/downloads_clear_completed"
_assert_status 401 "POST /downloads_clear_completed (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X DELETE -H "Authorization: Bearer $GUEST_TOKEN" \
		"$API/downloads/$TEST_HASH"
	_assert_status 403 "DELETE /downloads/{hash} (guest) → 403"
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$API/downloads_clear_completed"
	_assert_status 403 "POST /downloads_clear_completed (guest) → 403"
else
	echo "    info: no guest pass; admin-gate skipped"
fi

# --- 2. DELETE non-existent hash → 404. ----------------------------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "DELETE /downloads/{nonexistent} → 404"

# --- 3. Bulk clear_completed on a clean queue (no completed). ------
#
# Pre-seed: clear anything that's already completed so we measure
# from a known baseline. (The smoke is order-independent — 11-downloads-default-filter
# may have left a completed entry behind, or it may not have. We
# call clear once to baseline, then move on.)
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads_clear_completed"
_assert_status 200 "POST /downloads_clear_completed (baseline) → 200"
# The shared results envelope has no top-level `ok`: per-item success lives on
# each entry, so a partial outcome cannot be reported as a single boolean.
_assert_json_eq '.results | type' array 'clear_completed returns a results array'

# Second call: now nothing is completed. cleared must be 0.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads_clear_completed"
_assert_status 200 "POST /downloads_clear_completed (idempotent no-op) → 200"
# An empty `results` array is the no-op. It was `cleared: 0`, a shape only this
# endpoint used.
_assert_json_eq '.results | length' 0 'clear_completed second call cleared 0 entries'

# --- 4. DELETE on an active partfile (happy path + no-stale GET). --
#
# Add the Ubuntu ISO, wait for it to surface, DELETE it, immediate
# GET must 404. This pins the mutate-then-refresh invariant on the
# active-path EC_OP_PARTFILE_DELETE branch.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"links\":[\"$TEST_LINK\"]}" "$API/downloads"
_assert_status 202 "POST /downloads (Ubuntu ISO) → 202 (setup)"

APPEARED=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$API/downloads?status=all"
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
	"$API/downloads/$TEST_HASH"
# 204, no body: `hash` came from the URL and `ok` restated the status code.
_assert_status 204 "DELETE /downloads/{Ubuntu ISO hash} → 204"
_assert_body_empty 'DELETE sends no body'

# Immediate GET — no stale cache.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads/$TEST_HASH"
_assert_status 404 "IMMEDIATE GET after DELETE → 404 (no stale cache)"

# Same in list view — entry must be gone from the default response.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads?status=all"
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
	"$API/downloads/$TEST_HASH"
_assert_status 404 "DELETE the same hash twice → 404 second time"

# --- 6. POST clear_completed {hash:...} on non-existent → 404. ----
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"hash":"baadbaadbaadbaadbaadbaadbaadbaad"}' \
	"$API/downloads_clear_completed"
_assert_status 404 "POST clear_completed {hash:unknown} → 404"
_assert_json_eq '.error.code' not_found 'clear_completed {hash:unknown}.error.code'

# --- 7. POST clear_completed malformed body → 400. ----------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json at all' \
	"$API/downloads_clear_completed"
_assert_status 400 "POST clear_completed (malformed body) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"hash": 12345}' \
	"$API/downloads_clear_completed"
_assert_status 400 "POST clear_completed {hash: non-string} → 400"

# --- 8. POST clear_completed {hash:active-partfile} → 409. --------
#
# Re-add the Ubuntu ISO so we have a known active partfile, then
# call clear_completed with that hash. The handler must refuse with
# 409 not_completed because the entry is in m_filelist, not in
# m_completedDownloads.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"links\":[\"$TEST_LINK\"]}" "$API/downloads"
_assert_status 202 "POST /downloads (Ubuntu ISO re-add) → 202 (setup)"

APPEARED=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$API/downloads?status=all"
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
	"$API/downloads_clear_completed"
_assert_status 409 "POST clear_completed {hash:active-partfile} → 409"
_assert_json_eq '.error.code' not_completed \
	'clear_completed active hash .error.code == not_completed'

# Cleanup the active partfile.
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads/$TEST_HASH"
_assert_status 204 "Cleanup: DELETE /downloads/{Ubuntu ISO hash} (2nd) → 204"

# --- 9. Clear an existing completed entry without deleting its file.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/downloads?status=all"
COMPLETED_HASH=$(printf '%s' "$CURL_BODY" \
	| jq -r '[.downloads[] | select(.status == "completed") | .hash][0] // empty')
if [ -n "$COMPLETED_HASH" ]; then
	# clear_completed {hash:completed} → 200, exactly 1 cleared.
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"hash\":\"$COMPLETED_HASH\"}" \
		"$API/downloads_clear_completed"
	_assert_status 200 "POST clear_completed {hash:completed} → 200"
	_assert_json_eq '.results | length' 1 'per-hash clear_completed cleared exactly 1'
	_assert_json_eq '.results[0].id' "$COMPLETED_HASH" \
		'results[0].id echoes the input hash'
	_assert_json_eq '.results[0].ok' true 'results[0] reports success'

	# Second call → 404 (entry is gone).
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"hash\":\"$COMPLETED_HASH\"}" \
		"$API/downloads_clear_completed"
	_assert_status 404 "POST clear_completed {hash:already-cleared} → 404"
else
	_skip "POST clear_completed {hash:completed} → 200 (no completed entry)"
fi

# --- Summary. -----------------------------------------------------
_summary
