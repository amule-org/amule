#!/usr/bin/env bash
#
# amuleapi Phase 4a — read endpoints, /status only. Validates the
# refresher → state cache → handler chain end-to-end against a live
# amuled. The remaining 12 endpoints (downloads, uploads, shared,
# servers, kad, categories, logs/amule, logs/serverinfo, preferences,
# stats/tree, stats/graphs, search/results) land in subsequent
# sub-phases (4b/4c/4d); their phase scripts share this directory.
#
# Bring-up convention:
#   rm -rf /tmp/amuleapi-phase4 && mkdir -p /tmp/amuleapi-phase4
#   amuleapi --config-dir=/tmp/amuleapi-phase4 --host=127.0.0.1 \
#            --port=4712 --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-phase4 --host=127.0.0.1 \
#            --port=4712 --password=amule &
#   ./phase4.sh
#
# Environment:
#   HOST=localhost:4713          amuleapi endpoint
#   ADMIN_PASS=adminpass         plaintext admin password
#
# Exits 0 on success, 1 on assertion failure, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_phase4_body.XXXXXX)
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
	resp=$(curl -s --max-time 10 \
		-o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
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
	_die "jq is required. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi phase 4a smoke @ $HOST"

# --- 1. /status without auth → 401 unauthorized. -------------------
_curl "$HOST/api/v0/status"
_assert_status 401 "GET /api/v0/status (no creds) → 401"
_assert_json_eq '.error.code' unauthorized \
	'unauthenticated /status carries error.code=unauthorized'

# --- 2. Log in as admin and capture the bearer. --------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for /status tests"

# Give the refresher a beat to populate the cache after first launch
# (the first tick fires immediately on entering TextShell; even on a
# cold start the wait is sub-second).
sleep 2

# --- 3. /status with bearer → 200 + envelope shape. ----------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/status"
_assert_status 200 "GET /api/v0/status (admin bearer) → 200"

# Envelope metadata.
_assert_json_eq '.ec_connected | type' boolean \
	'ec_connected is boolean'

# ed2k subtree.
_assert_json_eq '.ed2k.state | test("^(connected|connecting|disconnected)$")' \
	true 'ed2k.state is a known enum value'
_assert_json_eq '.ed2k.low_id | type' boolean \
	'ed2k.low_id is boolean'
_assert_json_eq '.ed2k.server_name | type' string \
	'ed2k.server_name is string'

# kad subtree.
_assert_json_eq '.kad.state | test("^(connected|connecting|disabled)$")' \
	true 'kad.state is a known enum value'
_assert_json_eq '.kad.firewalled | type' boolean \
	'kad.firewalled is boolean'

# speeds + queue subtrees.
_assert_json_eq '.speeds.download_bps | type' number \
	'speeds.download_bps is numeric'
_assert_json_eq '.speeds.upload_bps | type' number \
	'speeds.upload_bps is numeric'
_assert_json_eq '.queue.upload_queue_length | type' number \
	'queue.upload_queue_length is numeric'
_assert_json_eq '.queue.total_source_count | type' number \
	'queue.total_source_count is numeric'

# --- 4. /status with guest bearer also works (any-role read gate). --
#       (no separate guest pass configured in this fixture; the
#       admin-only test above is enough to prove the auth gate works.)

# --- 5. Method gate. ----------------------------------------------
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/status"
_assert_status 405 "DELETE /api/v0/status → 405 method_not_allowed"

# --- 6. HEAD /status. ----------------------------------------------
_curl -I -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/status"
_assert_status 200 "HEAD /api/v0/status → 200"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
