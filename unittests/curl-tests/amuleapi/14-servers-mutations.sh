#!/usr/bin/env bash
#
# amuleapi 14-servers-mutations — server lifecycle mutations.
#
# Endpoints:
#   POST   /api/v0/servers                   — add by {address, name?}
#   POST   /api/v0/servers/{ecid}/connect    — connect to one server
#   PATCH  /api/v0/servers/{ecid}            — set priority / static flag
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

_assert_body_empty() {
	local label=$1
	if [ -z "$CURL_BODY" ]; then
		_pass "$label"
	else
		_fail "$label" "expected an empty body, got: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
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
# Accept 202 (accepted) OR 400 (already in list — server was added
# by a prior smoke or the operator). Both are valid endings.
#
# 202 with no body, not 201 + the created object: EC_OP_SERVER_ADD answers
# success or failure and never returns the server it made, so a body here
# could only be reconstructed from the snapshot after an inline refresh.
# The caller re-reads /servers, which it had to do anyway.
if [ "$CURL_STATUS" = "202" ]; then
	_pass "POST /servers (add tagged server) → 202"
	_assert_body_empty "POST /servers sends no body"
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
		"expected 202 or 400, got $CURL_STATUS" \
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
# (returns NOOP); the actual TCP connect is async. 202 Accepted, no body:
# `ecid` came from the URL and the outcome only shows up later on
# /status.ed2k.state and the SSE stream.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/$ECID/connect"
_assert_status 202 "POST /servers/{ecid}/connect → 202"
_assert_body_empty 'connect sends no body'

# Bad ECID → 400 (path can't parse), or 404 (parses but no match).
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/not-a-number/connect"
_assert_status 400 "POST /servers/not-a-number/connect → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/4294967295/connect"
_assert_status 404 "POST /servers/{unknown ecid}/connect → 404"

# --- 5b. PATCH /servers/{ecid} — priority + static (#692). ---------
# The SRV_PR_* wire values are not monotone (NORMAL=0, HIGH=1, LOW=2),
# so round-tripping every name through the API is what actually proves
# the reverse mapping, not just that a 200 came back.
for PRIO in high low normal; do
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"priority\":\"$PRIO\"}" "$HOST/api/v0/servers/$ECID"
	_assert_status 200 "PATCH /servers/{ecid} priority=$PRIO → 200"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/servers"
	GOT=$(printf '%s' "$CURL_BODY" | jq -r --argjson e "$ECID" \
		'.servers[] | select(.ecid == $e) | .priority')
	if [ "$GOT" = "$PRIO" ]; then
		_pass "priority=$PRIO round-trips through GET /servers"
	else
		_fail "priority round-trip" "set $PRIO, GET returned $GOT"
	fi
done

# static flag, both directions.
for FLAG in true false; do
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"static\":$FLAG}" "$HOST/api/v0/servers/$ECID"
	_assert_status 200 "PATCH /servers/{ecid} static=$FLAG → 200"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/servers"
	GOT=$(printf '%s' "$CURL_BODY" | jq -r --argjson e "$ECID" \
		'.servers[] | select(.ecid == $e) | .static')
	if [ "$GOT" = "$FLAG" ]; then
		_pass "static=$FLAG round-trips through GET /servers"
	else
		_fail "static round-trip" "set $FLAG, GET returned $GOT"
	fi
done

# Both fields in one body.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"high","static":true}' "$HOST/api/v0/servers/$ECID"
_assert_status 200 "PATCH /servers/{ecid} priority+static together → 200"
# The response is the full server object as it now stands, not an {ok} ack:
# a PATCH answers with the state the caller just produced, so no re-read is
# needed to see it.
_assert_json_eq '.ecid' "$ECID" 'PATCH response is the server object, keyed by ecid'
_assert_json_eq '.priority' high 'PATCH response carries the new priority'
_assert_json_eq '.static' true 'PATCH response carries the new static flag'
_assert_json_eq '. | has("ok")' false 'PATCH response has no constant ok field'

# --- 5c. PATCH error paths. ----------------------------------------
_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/servers/$ECID"
_assert_status 401 "PATCH /servers/{ecid} (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X PATCH -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"priority":"high"}' "$HOST/api/v0/servers/$ECID"
	_assert_status 403 "PATCH /servers/{ecid} (guest) → 403"
fi

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{}' "$HOST/api/v0/servers/$ECID"
_assert_status 400 "PATCH /servers/{ecid} empty body → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"urgent"}' "$HOST/api/v0/servers/$ECID"
_assert_status 400 "PATCH /servers/{ecid} unknown priority → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"static":"yes"}' "$HOST/api/v0/servers/$ECID"
_assert_status 400 "PATCH /servers/{ecid} non-bool static → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/servers/not-a-number"
_assert_status 400 "PATCH /servers/not-a-number → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/servers/999999"
_assert_status 404 "PATCH /servers/{unknown ecid} → 404 (EC no-ops silently, #692)"

# --- 6. DELETE /servers/{ecid} happy path + no-stale invariant. ---
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/$ECID"
# 204, no body: `ecid` came from the URL and `ok` restated the status code.
_assert_status 204 "DELETE /servers/$ECID → 204"
_assert_body_empty 'DELETE sends no body'

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

# --- 8. ip:port selector: malformed is 400, unknown is 404. --------
# /servers/by-address/{address} takes an "<ip>:<port>" selector where the
# ECID routes take a number. Parsing and lookup are separate outcomes: a
# selector that cannot be parsed is the caller's mistake (400), one that
# parses but names no server we hold is simply absent (404). They used to
# collapse onto the same 404 because the resolver signalled every failure by
# returning ECID 0.
# The address form has its own path now, so a colon is no longer what selects
# the handler: a value without one reaches the same place and is rejected for
# being a malformed address rather than for looking like an ECID.
for bad in "not-an-ip:4242" "1.2.3.4:" ":4242" "1.2.3.4:0" "1.2.3.4:70000" \
	"1.2.3.4:abc" "999.1.1.1:4242" "no-colon-at-all"; do
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/servers/by-address/$bad"
	_assert_status 400 "DELETE /servers/by-address/$bad (malformed address) → 400"
done

# Well-formed but absent: TEST-NET-1 (RFC 5737), never a real server.
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/by-address/192.0.2.1:4242"
_assert_status 404 "DELETE /servers/by-address/192.0.2.1:4242 (unknown server) → 404"
_assert_json_eq '.error.code' not_found \
	'unknown ip:port carries error.code=not_found'

# Same split on the other two routes that take the selector.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/by-address/not-an-ip:4242/connect"
_assert_status 400 "POST /servers/by-address/not-an-ip:4242/connect (malformed) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/servers/by-address/192.0.2.1:4242/connect"
_assert_status 404 "POST /servers/by-address/192.0.2.1:4242/connect (unknown) → 404"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{"static":true}' \
	"$HOST/api/v0/servers/by-address/not-an-ip:4242"
_assert_status 400 "PATCH /servers/by-address/not-an-ip:4242 (malformed) → 400"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
