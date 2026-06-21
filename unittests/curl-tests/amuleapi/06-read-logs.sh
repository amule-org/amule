#!/usr/bin/env bash
#
# amuleapi 06-read-logs — /logs/amule + /logs/serverinfo. amule log
# rides on STAT_REQ's `EC_TAG_STATS_LOGGER_MESSAGE` channel (per-EC-
# connection cursor, incremental); server-info log is full-snapshot
# via `EC_OP_GET_SERVERINFO`.
#
# Bring-up:
#   amuleapi --config-dir=/tmp/amuleapi-06-read-logs ... &
#   ./06-read-logs.sh

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_06_read_logs_body.XXXXXX)
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

echo "amuleapi 06-read-logs smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Give the refresher a few seconds to drain the initial log baseline
# (CLoggerAccess emits the entire pre-connection backlog on its first
# tick).
sleep 5

# --- 1. Auth gate. -------------------------------------------------
_curl "$HOST/api/v0/logs/amule"
_assert_status 401 "GET /logs/amule (no creds) → 401"
_curl "$HOST/api/v0/logs/serverinfo"
_assert_status 401 "GET /logs/serverinfo (no creds) → 401"

# --- 2. /logs/amule full response shape. ---------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/amule"
_assert_status 200 "GET /logs/amule → 200"
_assert_json_eq '.lines | type'         array  '/logs/amule.lines is an array'
_assert_json_eq '.total_cached | type'  number '/logs/amule.total_cached is numeric'
_assert_json_eq '.returned | type'      number '/logs/amule.returned is numeric'
_assert_json_eq '.lines | length > 0'   true   '/logs/amule.lines is non-empty (amule emits a banner on connect)'
# When no tail is given, returned == total_cached.
_assert_json_eq '(.returned == .total_cached)' true \
	'/logs/amule: returned == total_cached when no ?tail given'

TOTAL=$(printf '%s' "$CURL_BODY" | jq '.total_cached')

# --- 3. /logs/amule?tail=N. ----------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/amule?tail=2"
_assert_status 200 "GET /logs/amule?tail=2 → 200"
_assert_json_eq '.returned'           2      '/logs/amule?tail=2 returns 2 lines'
_assert_json_eq '.lines | length'     2      '/logs/amule?tail=2 array length is 2'
_assert_json_eq '.total_cached'       "$TOTAL" '/logs/amule?tail=2 still reports full cached count'

# tail=0 means "no tailing" → all lines. The wire contract makes 0
# the inert default (matches the helper's behaviour: 0 = unbounded).
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/amule?tail=0"
_assert_status 200 "GET /logs/amule?tail=0 → 200"
_assert_json_eq '(.returned == .total_cached)' true \
	'/logs/amule?tail=0 returns all (tail=0 is "no tailing")'

# Bogus / non-numeric tail clamps to 0 (return all).
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/amule?tail=notanumber"
_assert_status 200 "GET /logs/amule?tail=notanumber → 200"
_assert_json_eq '(.returned == .total_cached)' true \
	'/logs/amule?tail=<bogus> defaults to "no tailing"'

# --- 4. /logs/serverinfo shape. ------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/serverinfo"
_assert_status 200 "GET /logs/serverinfo → 200"
_assert_json_eq '.text | type'           string '/logs/serverinfo.text is string'
_assert_json_eq '.total_bytes | type'    number '/logs/serverinfo.total_bytes is numeric'
_assert_json_eq '.returned_bytes | type' number '/logs/serverinfo.returned_bytes is numeric'
_assert_json_eq '(.returned_bytes == .total_bytes)' true \
	'/logs/serverinfo: returned_bytes == total_bytes when no ?tail given'

TOTAL_BYTES=$(printf '%s' "$CURL_BODY" | jq '.total_bytes')

# --- 5. /logs/serverinfo?tail=3 — line-boundary slicing. -----------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/serverinfo?tail=3"
_assert_status 200 "GET /logs/serverinfo?tail=3 → 200"
_assert_json_eq '(.returned_bytes <= .total_bytes)' true \
	'/logs/serverinfo?tail=3: returned_bytes <= total_bytes'
_assert_json_eq '.total_bytes' "$TOTAL_BYTES" \
	'/logs/serverinfo?tail=3 reports the same total_bytes'

# --- 6. Method gate. -----------------------------------------------
# DELETE on /logs/{amule,serverinfo} now CLEARS the buffer (phase 11
# / RFC §4.11 alignment); the 405 contract this test originally
# asserted has been retired. PATCH stays a 405 — the logs are
# read+reset only, never partially mutable.
for ep in logs/amule logs/serverinfo; do
	_curl -X PATCH -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/$ep"
	_assert_status 405 "PATCH /api/v0/$ep → 405"
done

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
