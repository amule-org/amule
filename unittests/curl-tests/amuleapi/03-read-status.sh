#!/usr/bin/env bash
#
# amuleapi 03-read-status — read endpoints, /status only. Validates the
# refresher → state cache → handler chain end-to-end against a live
# amuled. The remaining 12 endpoints (downloads, uploads, shared,
# servers, kad, categories, logs/amule, logs/serverinfo, preferences,
# stats/tree, stats/graphs, search/results) land in subsequent
# sub-phases (4b/4c/4d); their phase scripts share this directory.
#
# Bring-up convention:
#   rm -rf /tmp/amuleapi-03-read-status && mkdir -p /tmp/amuleapi-03-read-status
#   amuleapi --config-dir=/tmp/amuleapi-03-read-status --host=127.0.0.1 \
#            --port=4712 --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-03-read-status --host=127.0.0.1 \
#            --port=4712 --password=amule &
#   ./03-read-status.sh
#
# Environment:
#   HOST=localhost:4713          amuleapi endpoint
#   ADMIN_PASS=adminpass         plaintext admin password
#   GUEST_PASS=guestpass         plaintext guest password (run-all.sh
#                                configures it via --set-guest-pass;
#                                standalone invocations need this set
#                                or the guest-read assertion is skipped)
#
# Exits 0 on success, 1 on assertion failure, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_03_read_status_body.XXXXXX)
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

echo "amuleapi 03-read-status smoke @ $HOST"

# --- 1. /status without auth → 401 unauthorized. -------------------
_curl "$HOST/api/v0/status"
_assert_status 401 "GET /api/v0/status (no creds) → 401"
_assert_json_eq '.error.code' unauthorized \
	'unauthenticated /status carries error.code=unauthorized'

# --- 2. Log in as admin and capture the bearer. --------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for /status tests"

# Wait for the refresher to land its first snapshot. On a fast
# Linux host the first tick is sub-second; on the Windows VM the
# 503 ec_unavailable window can stretch a few seconds under load.
# Poll up to 15 s, same shape run-all.sh uses for /version, so
# the test doesn't drift to "disconnected" under runner pressure.
for _ in $(seq 1 30); do
	probe=$(curl -s -o /dev/null -w "%{http_code}" \
		-H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/status")
	[ "$probe" = "200" ] && break
	sleep 0.5
done

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
GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
if [ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ]; then
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/status"
	_assert_status 200 "GET /api/v0/status (guest bearer) → 200"
else
	# run-all.sh always configures a guest password; if a future
	# fixture drops it, surface the gap rather than silently
	# pretending the guest read-gate was exercised.
	_die "guest login failed in 03-read-status fixture — 03-read-status is supposed to verify both roles can read /status; check that GUEST_PASS is wired"
fi

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
