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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
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
# Poll up to 15 s, same shape run-all.sh uses for /health, so
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
_assert_json_eq '.ed2k.high_id | type' boolean \
	'ed2k.high_id is boolean'
_assert_json_eq '.ed2k.user_id | type' number \
	'ed2k.user_id is numeric'
# The old spelling was a bare `id`, which read as a local handle rather than
# the server-assigned identity it actually is.
_assert_json_eq '.ed2k | has("id")' false \
	'ed2k.id is gone, replaced by ed2k.user_id'
_assert_json_eq '.ed2k.public_ip | type' string \
	'ed2k.public_ip is string'
# A public address exists exactly when we hold a HighID on a live connection;
# a LowID carries none, and neither does a disconnected daemon.
_assert_json_eq '(.ed2k.public_ip != "") == (.ed2k.high_id and .ed2k.state == "connected")' \
	true 'ed2k.public_ip is non-empty exactly when high_id and connected'
# The 0xffffffff "connect in flight" sentinel must never surface.
_assert_json_eq '.ed2k.user_id != 4294967295' true \
	'ed2k.user_id never reports the connecting sentinel'
_assert_json_eq '.ed2k.server_name | type' string \
	'ed2k.server_name is string'

# kad subtree.
_assert_json_eq '.kad.state | test("^(connected|connecting|disabled)$")' \
	true 'kad.state is a known enum value'
# Named for the transport: this is the TCP half of the pair GET /kad reports,
# not an overall verdict refined by firewalled_udp.
_assert_json_eq '.kad.firewalled_tcp | type' boolean \
	'kad.firewalled_tcp is boolean'
_assert_json_eq '.kad | has("firewalled")' false \
	'kad.firewalled is gone, replaced by kad.firewalled_tcp'

# speeds + queue subtrees.
_assert_json_eq '.speeds.download_bps | type' number \
	'speeds.download_bps is numeric'
_assert_json_eq '.speeds.upload_bps | type' number \
	'speeds.upload_bps is numeric'
_assert_json_eq '.speeds.download_overhead_bps | type' number \
	'speeds.download_overhead_bps is numeric'
_assert_json_eq '.speeds.upload_overhead_bps | type' number \
	'speeds.upload_overhead_bps is numeric'
_assert_json_eq '.queue.upload_clients_waiting | type' number \
	'queue.upload_clients_waiting is numeric'
_assert_json_eq '.queue.download_sources_total | type' number \
	'queue.download_sources_total is numeric'

# disk subtree: a number, or null when the daemon has no figure. Never the
# unsigned reading of the -1 sentinel, and never 0 (which would read as a
# full disk).
_assert_json_eq '.disk.temp_free_bytes | type | . == "number" or . == "null"' true \
	'disk.temp_free_bytes is a number or null'
_assert_json_eq '.disk.incoming_free_bytes | type | . == "number" or . == "null"' true \
	'disk.incoming_free_bytes is a number or null'
_assert_json_eq '[.disk[]] | map(select(. == 18446744073709551615)) | length == 0' true \
	'disk figures never report the unsigned free-space sentinel'

# These spellings are not part of the surface; assert they are absent from the
# whole body rather than merely unused by the assertions above.
_assert_json_eq '[paths | join(".")] | map(select(test("low_id|upload_queue_length|total_source_count"))) | length == 0' \
	true '/status carries none of these field names'

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
