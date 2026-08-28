#!/usr/bin/env bash
#
# amuleapi 34-friends — the /friends resource.
#
# The friends list is the one the desktop client keeps in emfriends.met.
# amuleapi serves it from the snapshot the refresher already builds: the
# daemon appends the whole list to every GET_UPDATE reply, so reading it
# costs no EC roundtrip of its own.
#
# What is asserted here:
#
#   * the envelope is the standard list shape — a `friends` array plus
#     `total` / `offset` / `limit` — and paginates through the shared
#     helpers rather than a private implementation,
#   * `limit` is clamped and a bad `limit` / `offset` / `order` is a 400,
#     the same contract as every other list endpoint,
#   * both advertised sort keys are accepted and an unknown one is a 400,
#   * a friend added by address round-trips: the POST is a bodyless 202
#     (EC's FRIEND op never returns the record it made) and the friend
#     then appears in the list with the address given, `online` false
#     while nothing is linked,
#   * `user_hash` is either empty or a 32-char lowercase MD4, so it
#     correlates with /clients `user_hash` directly,
#   * `friend_slot` toggles through PATCH and reads back, which is the
#     part that needed the daemon to serialize the flag at all,
#   * DELETE removes it (204, no body) and a second DELETE is a 404 -
#     the EC op is idempotent but a mistyped id must not answer 204,
#   * the mutations require ADMIN and the read does not,
#   * malformed bodies are rejected before any EC traffic,
#   * HEAD is routed like GET, and non-routed methods are 405.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   ./34-friends.sh
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0
AUTH=()

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
	resp=$(curl -s --max-time 10 -D /tmp/amuleapi_34_head -o /tmp/amuleapi_34_body \
		-w '%{http_code}' "${AUTH[@]}" "$@") || _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat /tmp/amuleapi_34_body)
	CURL_HEAD=$(cat /tmp/amuleapi_34_head)
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS" "body: ${CURL_BODY:0:200}"
	fi
}

_jq() { echo "$CURL_BODY" | jq -r "$1" 2>/dev/null; }

trap 'rm -f /tmp/amuleapi_34_head /tmp/amuleapi_34_body' EXIT

command -v jq >/dev/null 2>&1 || _die "jq is required"

if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 34-friends @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "could not log in for friends tests"
AUTH=(-H "Authorization: Bearer $TOKEN")

# --- 1. The envelope. ---------------------------------------------
_curl "$HOST/api/v0/friends"
_assert_status 200 "GET /friends"

if [ "$(_jq 'has("friends")')" = "true" ]; then
	_pass "envelope has a friends array"
else
	_fail "envelope" "no friends key in: ${CURL_BODY:0:200}"
fi

for k in total offset limit; do
	if [ "$(_jq "has(\"$k\")")" = "true" ]; then
		_pass "envelope has $k"
	else
		_fail "envelope" "missing pagination key: $k"
	fi
done

BEFORE=$(_jq '.total')
echo "        (list holds $BEFORE friend(s) before this run)"

# --- 2. Shared list-parameter contract. ----------------------------
_curl "$HOST/api/v0/friends?limit=1&offset=0"
_assert_status 200 "GET /friends?limit=1"
[ "$(_jq '.limit')" = "1" ] && _pass "limit is echoed" || _fail "limit echo" "got $(_jq '.limit')"

# Over the cap is a rejection, not a silent clamp. It used to answer 200 with a
# quietly reduced window, so a client asking for 99999 got 500 rows with nothing
# in the response saying the request had been altered.
_curl "$HOST/api/v0/friends?limit=1000000001"
_assert_status 400 "GET /friends?limit=1000000001 is rejected, not clamped"

# The cap itself is still valid.
_curl "$HOST/api/v0/friends?limit=500"
_assert_status 200 "GET /friends?limit=500 (the cap is in range)"

for bad in "limit=abc" "limit=-1" "offset=-1" "order=sideways"; do
	_curl "$HOST/api/v0/friends?$bad"
	_assert_status 400 "GET /friends?$bad is rejected"
done

for key in name online; do
	_curl "$HOST/api/v0/friends?sort=$key&order=desc"
	_assert_status 200 "sort=$key is accepted"
done
_curl "$HOST/api/v0/friends?sort=nonsuch"
_assert_status 400 "unknown sort key is rejected"

# --- 3. Add by address, then read it back. -------------------------
# 203.0.113.x is TEST-NET-3 (RFC 5737) — never routable, so this cannot
# reach a real peer no matter where the regtest daemon runs.
TEST_IP="203.0.113.42"
TEST_PORT=4662
_curl -X POST -H "Content-Type: application/json" \
	-d "{\"ip\":\"$TEST_IP\",\"port\":$TEST_PORT,\"name\":\"curltest-friend\"}" \
	"$HOST/api/v0/friends"
# 202 with no body. EC's FRIEND op answers success or failure and never
# returns the record it created, so the handler used to name the new friend by
# diffing the snapshot against a pre-add copy - the object when the inline
# refresh had landed, a bare {ok} when it had not. The caller re-reads
# /friends instead, which is what the rest of this phase does.
_assert_status 202 "POST /friends (address form)"
[ -z "$CURL_BODY" ] && _pass "POST /friends sends no body" \
	|| _fail "POST body" "expected empty, got: ${CURL_BODY:0:200}"

_curl "$HOST/api/v0/friends?limit=500"
AFTER=$(_jq '.total')

# Find the friend just added, by the address it was added with.
NEW_ECID=$(echo "$CURL_BODY" | jq -r --arg ip "$TEST_IP" --argjson p "$TEST_PORT" \
	'[.friends[] | select(.ip == $ip and .port == $p)] | first | .ecid // empty')
if [ -n "$NEW_ECID" ]; then
	_pass "the added friend is readable from /friends (ecid=$NEW_ECID)"
else
	_fail "friend lookup" "no friend at $TEST_IP:$TEST_PORT in: ${CURL_BODY:0:300}"
	_die "cannot continue without the new friend's ecid"
fi
NEW=$(echo "$CURL_BODY" | jq -r --argjson e "$NEW_ECID" \
	'[.friends[] | select(.ecid == $e)] | first')
[ "$(echo "$NEW" | jq -r .ip)" = "$TEST_IP" ] \
	&& _pass "ip round-trips" || _fail "ip" "got $(echo "$NEW" | jq -r .ip)"
[ "$(echo "$NEW" | jq -r .port)" = "$TEST_PORT" ] \
	&& _pass "port round-trips" || _fail "port" "got $(echo "$NEW" | jq -r .port)"
[ "$(echo "$NEW" | jq -r .online)" = "false" ] \
	&& _pass "a friend with no linked peer is offline" \
	|| _fail "online" "expected false, got $(echo "$NEW" | jq -r .online)"
# ...and reports that as null, not as the 0 sentinel it used to send. 0 is not
# how this surface spells "no value" anywhere else, and a client joining
# naively on the raw number was building GET /clients/0 and taking a 404.
[ "$(echo "$NEW" | jq -r '.client_ecid')" = "null" ] \
	&& _pass "an offline friend reports client_ecid null, not a 0 sentinel" \
	|| _fail "client_ecid" "expected null, got $(echo "$NEW" | jq -r .client_ecid)"
if [ "$AFTER" -gt "$BEFORE" ] 2>/dev/null; then
	_pass "the list grew ($BEFORE -> $AFTER)"
else
	_fail "list growth" "expected > $BEFORE, got $AFTER"
fi

# every record's hash is either absent-as-empty or a real MD4
BADHASH=$(echo "$CURL_BODY" | jq -r '[.friends[] | select(.user_hash != "" and (.user_hash | test("^[0-9a-f]{32}$") | not))] | length')
[ "$BADHASH" = "0" ] && _pass "user_hash is empty or 32-char lowercase MD4" \
	|| _fail "user_hash shape" "$BADHASH record(s) with a malformed hash"

# --- 4. The friend slot, the flag that needed the core change. -----
_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"friend_slot":true}' "$HOST/api/v0/friends/$NEW_ECID"
_assert_status 200 "PATCH friend_slot=true"
[ "$(_jq '.friend_slot')" = "true" ] && _pass "friend_slot reads back true" \
	|| _fail "friend_slot" "expected true, got $(_jq '.friend_slot') — is the daemon serializing the tag?"

_curl "$HOST/api/v0/friends?limit=500"
STILL=$(echo "$CURL_BODY" | jq -r --arg e "$NEW_ECID" '[.friends[] | select(.ecid == ($e|tonumber)) | .friend_slot] | first')
[ "$STILL" = "true" ] && _pass "the slot survives into the list view" \
	|| _fail "friend_slot in list" "expected true, got $STILL"

_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"friend_slot":false}' "$HOST/api/v0/friends/$NEW_ECID"
_assert_status 200 "PATCH friend_slot=false"
[ "$(_jq '.friend_slot')" = "false" ] && _pass "the slot clears again" \
	|| _fail "friend_slot" "expected false, got $(_jq '.friend_slot')"

_curl -X PATCH -H "Content-Type: application/json" -d '{}' "$HOST/api/v0/friends/$NEW_ECID"
_assert_status 400 "PATCH with no recognized field is rejected"

# --- 5. Bad input is rejected before any EC traffic. ---------------
_curl -X POST -H "Content-Type: application/json" \
	-d '{"client_ecid":1,"ip":"203.0.113.9","port":4662}' "$HOST/api/v0/friends"
_assert_status 400 "POST with both body forms is rejected"

_curl -X POST -H "Content-Type: application/json" \
	-d '{"ip":"not-an-ip","port":4662}' "$HOST/api/v0/friends"
_assert_status 400 "POST with a malformed ip is rejected"

_curl -X POST -H "Content-Type: application/json" \
	-d "{\"ip\":\"$TEST_IP\",\"port\":0}" "$HOST/api/v0/friends"
_assert_status 400 "POST with a zero port is rejected"

_curl -X POST -H "Content-Type: application/json" \
	-d "{\"ip\":\"$TEST_IP\",\"port\":4662,\"user_hash\":\"nothex\"}" "$HOST/api/v0/friends"
_assert_status 400 "POST with a malformed user_hash is rejected"

_curl -X POST -H "Content-Type: application/json" \
	-d '{"client_ecid":4294967290}' "$HOST/api/v0/friends"
_assert_status 404 "POST naming an unknown client_ecid is a 404"

# --- 6. HEAD and method routing. -----------------------------------
# Status only, as the other phases do: `curl -I` prints the response headers
# on stdout, so the -o capture holds those rather than a body, and asserting
# emptiness here would be testing curl instead of the endpoint.
_curl -I "$HOST/api/v0/friends"
_assert_status 200 "HEAD /friends"

_curl -X PUT "$HOST/api/v0/friends"
_assert_status 405 "PUT /friends is 405"
_curl -X POST "$HOST/api/v0/friends/$NEW_ECID"
_assert_status 405 "POST /friends/{ecid} is 405"

# --- 7. Guests read but cannot mutate. ------------------------------
GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
if [ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ]; then
	SAVED=("${AUTH[@]}")
	AUTH=(-H "Authorization: Bearer $GUEST_TOKEN")
	_curl "$HOST/api/v0/friends"
	_assert_status 200 "guest may GET /friends"
	_curl -X DELETE "$HOST/api/v0/friends/$NEW_ECID"
	_assert_status 403 "guest may not DELETE a friend"
	_curl -X POST -H "Content-Type: application/json" \
		-d "{\"ip\":\"$TEST_IP\",\"port\":4662}" "$HOST/api/v0/friends"
	_assert_status 403 "guest may not POST a friend"
	AUTH=("${SAVED[@]}")
else
	echo "        (no guest account configured — skipping the guest assertions)"
fi

# --- 8. Remove, and prove a stale id does not answer 200. ----------
_curl -X DELETE "$HOST/api/v0/friends/$NEW_ECID"
# 204, no body: the ecid came from the URL and `ok` restated the status code.
_assert_status 204 "DELETE /friends/$NEW_ECID"
[ -z "$CURL_BODY" ] && _pass "DELETE sends no body" \
	|| _fail "DELETE body" "expected empty, got: ${CURL_BODY:0:200}"

_curl -X DELETE "$HOST/api/v0/friends/$NEW_ECID"
_assert_status 404 "a second DELETE of the same id is a 404"

_curl "$HOST/api/v0/friends"
FINAL=$(_jq '.total')
[ "$FINAL" = "$BEFORE" ] && _pass "the list is back to its starting size ($BEFORE)" \
	|| _fail "cleanup" "expected $BEFORE, got $FINAL"

# --- Summary -------------------------------------------------------
echo
echo "34-friends: $((TEST_COUNT-FAIL_COUNT))/$TEST_COUNT assertions passed"
[ "$FAIL_COUNT" -eq 0 ] || exit 1
