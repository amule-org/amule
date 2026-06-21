#!/usr/bin/env bash
#
# amuleapi 16-networks-connect — connection control mutations.
#
# Endpoints:
#   POST /api/v0/networks/connect       — EC_OP_CONNECT (all enabled
#                                         nets) or one of
#                                         EC_OP_SERVER_CONNECT / EC_OP_KAD_START
#                                         when a network selector is passed
#   POST /api/v0/networks/disconnect    — EC_OP_DISCONNECT (all nets)
#   (Dedicated /api/v0/kad/{connect,disconnect} were dropped in
#   favour of the network-selector form on /networks/*.)
#   POST /api/v0/kad/bootstrap          — EC_OP_KAD_BOOTSTRAP_FROM_IP
#       body: {ip: "1.2.3.4" | uint32, port: uint16}
#
# amuled's CONNECT/DISCONNECT return EC_OP_STRINGS with status
# messages — the handler relays those into `response.message`.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_16_networks_connect_body.XXXXXX)
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

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 16-networks-connect smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X POST "$HOST/api/v0/networks/disconnect"
_assert_status 401 "POST /networks/disconnect (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/networks/disconnect"
	_assert_status 403 "POST /networks/disconnect (guest) → 403"
fi

# --- 2. networks/disconnect → 200 + message. -----------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/networks/disconnect"
_assert_status 200 "POST /networks/disconnect → 200"
_assert_json_eq '.ok' true 'disconnect response.ok==true'

# --- 3. networks/connect → 202 + message. --------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/networks/connect"
_assert_status 202 "POST /networks/connect → 202"
_assert_json_eq '.ok' true 'connect response.ok==true'
_assert_json_eq '.message | type' string 'connect response carries .message'

# --- 4. networks/{disconnect,connect} (Kad-only via selector). ----
# The dedicated /kad/connect + /kad/disconnect endpoints were dropped
# in favour of /networks/{connect,disconnect} with `{"network":"kad"}`.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"network":"kad"}' \
	"$HOST/api/v0/networks/disconnect"
_assert_status 200 "POST /networks/disconnect {network:kad} → 200"
_assert_json_eq '.ok' true 'networks/disconnect(kad) response.ok==true'

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"network":"kad"}' \
	"$HOST/api/v0/networks/connect"
_assert_status 202 "POST /networks/connect {network:kad} → 202"
_assert_json_eq '.ok' true 'networks/connect(kad) response.ok==true'

# ed2k-only selector should also round-trip via the network field.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"network":"ed2k"}' \
	"$HOST/api/v0/networks/connect"
_assert_status 202 "POST /networks/connect {network:ed2k} → 202"

# Bogus selector → 400 on both directions.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"network":"wat"}' \
	"$HOST/api/v0/networks/connect"
_assert_status 400 "POST /networks/connect {network:wat} → 400"

# --- 5. kad/bootstrap happy path + error paths. -------------------
#
# Bootstrap to a localhost dummy address — amuled doesn't validate
# routability; the call always succeeds at the EC-handler level (the
# actual Kad probe is fire-and-forget UDP).
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ip":"127.0.0.1","port":4672}' \
	"$HOST/api/v0/kad/bootstrap"
_assert_status 202 "POST /kad/bootstrap (dotted-quad) → 202"
_assert_json_eq '.ok'   true   'kad/bootstrap response.ok==true'
_assert_json_eq '.port' 4672   'kad/bootstrap response echoes port'

# Uint32 IP form should also work.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ip":2130706433,"port":4672}' \
	"$HOST/api/v0/kad/bootstrap"
_assert_status 202 "POST /kad/bootstrap (uint32 IP) → 202"

# Error: missing port.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ip":"127.0.0.1"}' "$HOST/api/v0/kad/bootstrap"
_assert_status 400 "POST /kad/bootstrap (no port) → 400"

# Error: bogus IP.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ip":"not.an.ip.addr","port":4672}' "$HOST/api/v0/kad/bootstrap"
_assert_status 400 "POST /kad/bootstrap (bad IP) → 400"

# Error: port out of range.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ip":"127.0.0.1","port":99999}' "$HOST/api/v0/kad/bootstrap"
_assert_status 400 "POST /kad/bootstrap (port>65535) → 400"

# --- 6. Method gates. ----------------------------------------------
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/networks/connect"
_assert_status 405 "GET /networks/connect → 405"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/kad/bootstrap"
_assert_status 405 "DELETE /kad/bootstrap → 405"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
