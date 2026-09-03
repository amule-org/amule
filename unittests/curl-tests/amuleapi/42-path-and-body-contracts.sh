#!/usr/bin/env bash
#
# amuleapi 42-path-and-body-contracts — the two contracts that are stated
# once and have to hold everywhere.
#
# 1. A `{hash}` that is not 32 hex characters is a MALFORMED REQUEST, so it
#    answers `400 bad_request`, on every route that takes one. The
#    find-based routes used to skip the format check and fall through to
#    their own `404`, which left a client unable to tell "not a hash" from
#    "valid hash, no such file" -- and the split ran through a single route:
#    `GET /search/results/{hash}/comments` answered `404` where its own
#    `POST` answered `400`. `{ecid}` has always drawn this line
#    (`RequireEcidPath`); these are its hash twin.
#
# 2. A body field documented as an integer rejects a fractional value
#    instead of truncating it. JSON has one number type, so `2.9` reaches
#    the handler as a double and a bare cast turns a filter the client
#    thinks it set into a different one.
#
# The `404` half of contract 1 is asserted too: a well-formed hash that
# names nothing must still be `404`, or the guard would have turned every
# missing file into a bad request.
set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

# Well-formed, and nothing on this surface can hold it: 32 hex characters.
ABSENT_HASH=00000000000000000000000000000000
# Malformed three ways: too short, non-hex characters, right length but
# with a non-hex character in it.
SHORT_HASH=deadbeef
NONHEX_HASH=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
ALMOST_HASH=0000000000000000000000000000000g

FAIL_COUNT=0
TEST_COUNT=0
# Counted apart from TEST_COUNT: a skipped check is coverage that did not
# happen, and folding it into the passed tally would report the absence of
# a check as a check that succeeded.
SKIP_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_42_contracts_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}
_skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP  $1"; }

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

# A bare 400 does not prove the validator rejected the body: a daemon with
# its networks disabled answers `400 amuled_rejected` to a perfectly
# well-formed search or bootstrap. Every body-contract check below asserts
# the error CODE too, so it means the same thing on a connected daemon and
# on an idle one.
_assert_bad_request() {
	local label=$1
	_assert_status 400 "$label"
	_assert_json_eq '.error.code' bad_request "$label names bad_request"
}

_assert_json_eq() {
	local expr=$1 expected=$2 label=$3
	local actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null) \
		|| { _fail "$label" "body was not valid JSON" "body: $CURL_BODY"; return; }
	if [ "$actual" = "$expected" ]; then
		_pass "$label"
	else
		_fail "$label" "expected $expected, got $actual" "body: $CURL_BODY"
	fi
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi

echo "amuleapi 42-path-and-body-contracts smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"
AUTH="Authorization: Bearer $ADMIN_TOKEN"

# --- 1. Malformed {hash} → 400 on every route that takes one. -----
#
# GET routes first, then the mutations. A mutation with a malformed hash
# cannot have a side effect: the guard runs before any lookup, which is
# also why it is safe to fire DELETE here.
echo
echo "  -- malformed {hash} → 400 --"

for h in "$SHORT_HASH" "$NONHEX_HASH" "$ALMOST_HASH"; do
	_curl -H "$AUTH" "$HOST/api/v0/downloads/$h"
	_assert_status 400 "GET /downloads/$h"
	_assert_json_eq '.error.code' bad_request "GET /downloads/$h names bad_request"
done

_curl -H "$AUTH" "$HOST/api/v0/downloads/$NONHEX_HASH/comments"
_assert_status 400 "GET /downloads/{bad}/comments"

_curl -H "$AUTH" "$HOST/api/v0/shared/$NONHEX_HASH"
_assert_status 400 "GET /shared/{bad}"

_curl -X PATCH -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/downloads/$NONHEX_HASH"
_assert_status 400 "PATCH /downloads/{bad}"

_curl -X PATCH -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/shared/$NONHEX_HASH"
_assert_status 400 "PATCH /shared/{bad}"

_curl -X DELETE -H "$AUTH" "$HOST/api/v0/downloads/$NONHEX_HASH"
_assert_status 400 "DELETE /downloads/{bad}"

# The route the split ran through: both methods on one path now agree.
_curl -H "$AUTH" "$HOST/api/v0/search/results/$NONHEX_HASH/comments"
_assert_status 400 "GET /search/results/{bad}/comments"
_curl -X POST -H "$AUTH" "$HOST/api/v0/search/results/$NONHEX_HASH/comments"
_assert_status 400 "POST /search/results/{bad}/comments (was already 400)"

# --- 2. A well-formed hash that names nothing is still 404. -------
#
# The guard must reject the shape, not the lookup. If this ever turns into
# a 400, "no such file" has been collapsed into "bad request" and the
# distinction the section above exists for is gone in the other direction.
echo
echo "  -- well-formed but absent {hash} → 404 --"

_curl -H "$AUTH" "$HOST/api/v0/downloads/$ABSENT_HASH"
_assert_status 404 "GET /downloads/{absent}"
_curl -H "$AUTH" "$HOST/api/v0/shared/$ABSENT_HASH"
_assert_status 404 "GET /shared/{absent}"
_curl -H "$AUTH" "$HOST/api/v0/downloads/$ABSENT_HASH/comments"
_assert_status 404 "GET /downloads/{absent}/comments"

# --- 3. Integer body fields reject a fractional value. ------------
echo
echo "  -- fractional value where an integer is documented → 400 --"

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"query":"contract-probe","min_size_bytes":2.9}' "$HOST/api/v0/search"
_assert_bad_request "POST /search (min_size_bytes 2.9)"
_assert_json_eq '.error.message | test("integer")' true \
	'the min_size_bytes 400 says an integer is wanted'

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"query":"contract-probe","max_size_bytes":9.5}' "$HOST/api/v0/search"
_assert_bad_request "POST /search (max_size_bytes 9.5)"

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"query":"contract-probe","min_source_count":1.5}' "$HOST/api/v0/search"
_assert_bad_request "POST /search (min_source_count 1.5)"

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"ip":"127.0.0.1","port":4672.5}' "$HOST/api/v0/kad/bootstrap"
_assert_bad_request "POST /kad/bootstrap (fractional port)"

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"ip":"203.0.113.42","port":4662.5}' "$HOST/api/v0/friends"
_assert_bad_request "POST /friends (fractional port)"

# --- 4. One port contract on both routes that take one. -----------
#
# 0 used to be accepted by /kad/bootstrap and rejected by /friends. No Kad
# contact is reachable on port 0, so the probe was sent and went nowhere.
echo
echo "  -- port 0 → 400 on both routes --"

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"ip":"127.0.0.1","port":0}' "$HOST/api/v0/kad/bootstrap"
_assert_bad_request "POST /kad/bootstrap (port 0)"
_assert_json_eq '.error.message | test("1\\.\\.65535")' true \
	'the kad/bootstrap 400 quotes the shared 1..65535 range'

_curl -X POST -H "$AUTH" -H "Content-Type: application/json" \
	-d '{"ip":"203.0.113.42","port":0}' "$HOST/api/v0/friends"
_assert_bad_request "POST /friends (port 0)"
_assert_json_eq '.error.message | test("1\\.\\.65535")' true \
	'the friends 400 quotes the same range'

# --- 5. Absence is spelled null, not a zero or an empty string. ---
#
# State-dependent, so each check skips when the daemon is not in the shape
# that exercises it rather than asserting something it cannot know.
echo
echo "  -- absence reads as null --"

_curl -H "$AUTH" "$HOST/api/v0/kad"
KAD_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.state')
if [ "$KAD_STATE" = "disabled" ]; then
	_assert_json_eq '.node_id' null 'GET /kad node_id is null while Kad is stopped'
else
	_skip "GET /kad node_id null check: Kad state is \"$KAD_STATE\", not disabled"
fi

_curl -H "$AUTH" "$HOST/api/v0/clients?limit=50"
NOADDR_ECID=$(printf '%s' "$CURL_BODY" | jq -r 'first(.clients[]? | select(.ip == null) | .ecid) // empty')
if [ -n "$NOADDR_ECID" ]; then
	_curl -H "$AUTH" "$HOST/api/v0/clients/$NOADDR_ECID"
	_assert_json_eq '.kad_port' null 'client detail nulls kad_port with ip/port'
else
	_skip 'client detail kad_port check: every connected client has an address'
fi

# `last_received_at` is the partfile's last-changed stamp, which amuled sets
# even for a download that has transferred nothing -- so "0 bytes received"
# is NOT the never-case and cannot be used to find one. What the contract
# forbids is the raw 0 itself: the serializer maps it to null, because a
# unix 0 reads as 1970 rather than as "never". Assert that no row carries
# one, which holds on any daemon state and fails the moment the mapping is
# dropped.
_curl -H "$AUTH" "$HOST/api/v0/downloads?limit=200"
_assert_json_eq '[.downloads[]? | select(.last_received_at == 0)] | length' 0 \
	'no download reports last_received_at as a raw 0 (the never-case is null)'

# --- Summary. -----------------------------------------------------
echo
SKIP_NOTE=""
[ "$SKIP_COUNT" -gt 0 ] && SKIP_NOTE=" ($SKIP_COUNT check(s) skipped -- see the SKIP lines above for why)"
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed$SKIP_NOTE"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
