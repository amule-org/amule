#!/usr/bin/env bash
#
# amuleapi 14-servers-mutations — server lifecycle mutations.
#
# Endpoints:
#   POST   /api/v0/servers                   — add by {address, name?}
#   POST   /api/v0/servers/{ecid}/connect    — connect to one server
#   DELETE /api/v0/servers/{ecid}            — remove from the list
#
# All keyed by ECID on the URL — the EC ops (CONNECT/REMOVE) actually
# identify the server by IPv4+port server-side, so the handler looks
# up the cache entry by ECID and builds the EC_TAG_SERVER tag from the
# cached IP+port. Phase 5c also fixed a latent /servers[].address
# bug: the GET_UPDATE (Phase 4f) per-server tag carries IP/port in
# CHILD tags (EC_TAG_SERVER_IP + EC_TAG_SERVER_PORT) rather than the
# outer-tag IPv4 shape the legacy GET_SERVER_LIST used. /servers[]
# was showing "0.0.0.0:0" for every entry; smoke pins the fix.
#
# Test server: a real eMule server (185.65.45.144:4232 = eDonkey
# Sicherheit) the operator added to amuled's serverlist before
# starting the smoke. We POST a duplicate to test the
# amuled-rejected error path, then DELETE the original.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

# Test server (host:port). The operator's daemon has a few dozen
# servers configured; the smoke adds a name we can target by string
# search and cleans it up at the end.
TEST_ADDRESS="185.65.45.144:4232"
TEST_NAME="14-servers-mutations-smoke-tag"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_14_servers_mutations_body.XXXXXX)
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

echo "amuleapi 14-servers-mutations smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. /servers[] address parse fix — must not be "0.0.0.0:0". ---
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/servers"
_assert_status 200 "GET /servers → 200"

# The operator's daemon has servers in its list with real IPs.
# Smoke would pass on an empty list (no servers means no parse path
# to exercise), but if there ARE servers, NONE should report the
# all-zeros sentinel — that was the GET_UPDATE child-tag parse bug.
N=$(printf '%s' "$CURL_BODY" | jq '.servers | length')
if [ "$N" -gt 0 ]; then
	BOGUS=$(printf '%s' "$CURL_BODY" | jq \
		'[.servers[] | select(.address == "0.0.0.0:0")] | length')
	if [ "$BOGUS" = "0" ]; then
		_pass "/servers[].address parses real IP:port (no \"0.0.0.0:0\" entries, $N servers checked)"
	else
		_fail "/servers[].address parse regression" \
			"$BOGUS / $N entries report \"0.0.0.0:0\""
	fi
	# `ecid` field must be present (URL key for the mutation endpoints).
	BAD_ECID=$(printf '%s' "$CURL_BODY" | jq '[.servers[] | select(.ecid == null)] | length')
	if [ "$BAD_ECID" = "0" ]; then
		_pass "/servers[].ecid populated for every entry"
	else
		_fail "/servers[].ecid missing" \
			"$BAD_ECID entries lack the ecid field"
	fi
else
	echo "    info: daemon has 0 servers configured; parse-bug check skipped"
fi

# --- 2. Auth + admin gate. -----------------------------------------
_curl -X POST -H "Content-Type: application/json" \
	-d "{\"address\":\"$TEST_ADDRESS\"}" "$HOST/api/v0/servers"
_assert_status 401 "POST /servers (no token) → 401"

_curl -X DELETE "$HOST/api/v0/servers/1"
_assert_status 401 "DELETE /servers/{ecid} (no token) → 401"

_curl -X POST "$HOST/api/v0/servers/1/connect"
_assert_status 401 "POST /servers/{ecid}/connect (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"address\":\"$TEST_ADDRESS\"}" "$HOST/api/v0/servers"
	_assert_status 403 "POST /servers (guest) → 403"
	_curl -X DELETE -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/servers/1"
	_assert_status 403 "DELETE /servers/{ecid} (guest) → 403"
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/servers/1/connect"
	_assert_status 403 "POST /servers/{ecid}/connect (guest) → 403"
else
	echo "    info: no guest pass; admin-gate skipped"
fi

# --- 3. POST /servers happy path — add the tagged server. ---------
#
# amuled treats duplicate (host:port) adds as rejections; if the
# server already exists with the same address, we'll get 400
# amuled_rejected. Strip any prior tag of the same name first so the
# smoke is idempotent (no DELETE in the orchestrator yet for older
# entries).
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"address\":\"$TEST_ADDRESS\",\"name\":\"$TEST_NAME\"}" \
	"$HOST/api/v0/servers"
# Accept 201 (fresh add) OR 400 (already in list — server was added
# by a prior smoke or the operator). Both are valid endings.
if [ "$CURL_STATUS" = "201" ]; then
	_pass "POST /servers (add tagged server) → 201"
elif [ "$CURL_STATUS" = "400" ]; then
	ERR_CODE=$(printf '%s' "$CURL_BODY" | jq -r '.error.code')
	if [ "$ERR_CODE" = "amuled_rejected" ]; then
		_pass "POST /servers (already in list) → 400 amuled_rejected"
	else
		_fail "POST /servers" \
			"got 400 with unexpected error.code=$ERR_CODE"
	fi
else
	_fail "POST /servers" \
		"expected 201 or 400, got $CURL_STATUS" \
		"body: $CURL_BODY"
fi

# Wait for the new server to land in cache (no inline refresh needed
# — POST handler runs RefresherTick before returning).
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/servers"
ECID=$(printf '%s' "$CURL_BODY" \
	| jq -r --arg n "$TEST_NAME" \
	  '[.servers[] | select(.name == $n)] | first | .ecid // empty')
if [ -n "$ECID" ] && [ "$ECID" != "null" ]; then
	_pass "Tagged server present in /servers (ecid=$ECID)"
else
	_fail "Tagged server lookup" \
		"could not find server with name=$TEST_NAME in /servers"
	_die "cannot continue without the test server ECID"
fi

# --- 4. POST /servers error paths. ---------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/servers"
_assert_status 400 "POST /servers (no address) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"address":"no-colon"}' "$HOST/api/v0/servers"
_assert_status 400 "POST /servers (no colon in address) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json' "$HOST/api/v0/servers"
_assert_status 400 "POST /servers (malformed JSON) → 400"

# --- 5. POST /servers/{ecid}/connect. ------------------------------
#
# This kicks off an ed2k connect attempt. amuled accepts the command
# (returns NOOP); the actual TCP connect is async. 202 Accepted.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/$ECID/connect"
_assert_status 202 "POST /servers/{ecid}/connect → 202"
_assert_json_eq '.ok' true 'connect response.ok==true'

# Bad ECID → 400 (path can't parse), or 404 (parses but no match).
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/not-a-number/connect"
_assert_status 400 "POST /servers/not-a-number/connect → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/4294967295/connect"
_assert_status 404 "POST /servers/{unknown ecid}/connect → 404"

# --- 6. DELETE /servers/{ecid} happy path + no-stale invariant. ---
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/$ECID"
_assert_status 200 "DELETE /servers/$ECID → 200"
_assert_json_eq '.ok'   true        'DELETE response.ok==true'
_assert_json_eq '.ecid' "$ECID"     'DELETE response echoes ecid'

# Immediate GET — entry must be gone from the cache.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/servers"
STILL_THERE=$(printf '%s' "$CURL_BODY" \
	| jq --arg n "$TEST_NAME" \
	  '[.servers[] | select(.name == $n)] | length')
if [ "$STILL_THERE" = "0" ]; then
	_pass "/servers no longer contains the deleted tagged server (no stale cache)"
else
	_fail "/servers staleness after DELETE" \
		"$TEST_NAME still present after DELETE"
fi

# --- 7. DELETE error paths. ----------------------------------------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/$ECID"
_assert_status 404 "DELETE /servers/{just-deleted ecid} → 404"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/not-a-number"
_assert_status 400 "DELETE /servers/not-a-number → 400"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
