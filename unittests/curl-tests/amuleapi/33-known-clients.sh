#!/usr/bin/env bash
#
# amuleapi 33-known-clients — GET /known_clients.
#
# /clients answers "who am I connected to right now", keyed by ECID.
# This route answers "who have I ever exchanged data with", keyed by the
# peer's user hash and read from the daemon's credit store, so it
# survives the daemon process any ECID belonged to.
#
# What is asserted here:
#
#   * the envelope is the standard list shape — a `known_clients` array
#     plus `total` / `offset` / `limit` — and paginates through the
#     shared helpers, not a private implementation,
#   * `limit` is clamped and a bad `limit` / `offset` / `order` is a 400,
#     same contract as every other list endpoint,
#   * every `sort` key the endpoint advertises is accepted and an
#     unknown one is a 400,
#   * each record carries the fields that are always recorded (user_hash,
#     the totals, last_seen_at, online) and omits — rather than empties —
#     the ones a pre-metadata record has no value for,
#   * `user_hash` is a 32-char lowercase MD4, so it correlates with
#     /clients `user_hash` directly,
#   * the store is maintained rather than re-read: a connected peer's row
#     tracks the live client state, and a peer met since the first request
#     is added rather than waiting for a refetch,
#   * the response is cacheable (ETag) and honours If-None-Match,
#   * HEAD returns headers with no body,
#   * non-safe methods are 405.
#
# Note on an empty store: a fresh regtest daemon has met no peers, so
# `total` is legitimately 0 and the per-record assertions skip
# themselves rather than fail. Point the daemon at a config dir with a
# populated clients.met to exercise them.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   ./33-known-clients.sh
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0
# Skips are counted apart from TEST_COUNT, never folded into it: a skipped
# section is coverage that did not happen, and adding it to the passed tally
# would report the absence of a check as a check that succeeded.
SKIP_COUNT=0
AUTH=()

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	local resp
	resp=$(curl -s --max-time 10 -D /tmp/amuleapi_33_head -o /tmp/amuleapi_33_body \
		-w '%{http_code}' "${AUTH[@]}" "$@") || _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat /tmp/amuleapi_33_body)
	CURL_HEAD=$(cat /tmp/amuleapi_33_head)
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS"
	fi
}

_header() { echo "$CURL_HEAD" | awk -F': ' -v k="$1" 'tolower($1) == k {gsub(/\r/,""); print $2}'; }

_jq() { echo "$CURL_BODY" | jq -r "$1" 2>/dev/null; }

trap 'rm -f /tmp/amuleapi_33_head /tmp/amuleapi_33_body' EXIT

command -v jq >/dev/null 2>&1 || _die "jq is required"

if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 33-known-clients @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for known-clients tests"
AUTH=(-H "Authorization: Bearer $TOKEN")

# --- 1. The envelope. ---------------------------------------------
_curl "$HOST/api/v0/known_clients"
_assert_status 200 "GET /known_clients"

if [ "$(_jq 'has("known_clients")')" = "true" ]; then
	_pass "envelope has a known_clients array"
else
	_fail "envelope" "no known_clients key in: ${CURL_BODY:0:200}"
fi

for k in total offset limit; do
	if [ "$(_jq "has(\"$k\")")" = "true" ]; then
		_pass "envelope has $k"
	else
		_fail "envelope" "missing pagination key: $k"
	fi
done

TOTAL=$(_jq '.total')
echo "        (store holds $TOTAL record(s))"

# --- 2. Pagination, through the shared helpers. -------------------
_curl "$HOST/api/v0/known_clients?limit=1"
_assert_status 200 "GET /known_clients?limit=1"
N=$(_jq '.known_clients | length')
if [ "${N:-0}" -le 1 ]; then
	_pass "limit=1 returns at most one record (got ${N:-0})"
else
	_fail "limit=1" "returned $N records"
fi

_curl "$HOST/api/v0/known_clients?limit=1&offset=99999999"
_assert_status 200 "GET /known_clients with a far offset"
N=$(_jq '.known_clients | length')
if [ "${N:-1}" -eq 0 ]; then
	_pass "an offset past the end returns an empty page, not an error"
else
	_fail "far offset" "expected 0 records, got $N"
fi

for bad in "limit=abc" "limit=99999999999" "offset=-1" "order=sideways"; do
	_curl "$HOST/api/v0/known_clients?$bad"
	_assert_status 400 "GET /known_clients?$bad is rejected"
done

# --- 3. Sort keys. -------------------------------------------------
for key in name software first_seen_at last_seen_at session_count uploaded_bytes_total downloaded_bytes_total; do
	_curl "$HOST/api/v0/known_clients?sort=$key&limit=1"
	_assert_status 200 "sort=$key is accepted"
done
_curl "$HOST/api/v0/known_clients?sort=nonsense"
_assert_status 400 "an unknown sort key is rejected"

_curl "$HOST/api/v0/known_clients?sort=last_seen_at&order=desc&limit=1"
_assert_status 200 "sort=last_seen_at&order=desc is accepted"

# --- 4. Record shape. ----------------------------------------------
if [ "${TOTAL:-0}" -gt 0 ]; then
	_curl "$HOST/api/v0/known_clients?limit=1"

	for k in user_hash uploaded_bytes_total downloaded_bytes_total last_seen_at online; do
		if [ "$(_jq ".known_clients[0] | has(\"$k\")")" = "true" ]; then
			_pass "record always carries $k"
		else
			_fail "record shape" "missing always-present key: $k"
		fi
	done

	HASH=$(_jq '.known_clients[0].user_hash')
	if echo "$HASH" | grep -qE '^[0-9a-f]{32}$'; then
		_pass "user_hash is a 32-char lowercase MD4 ($HASH)"
	else
		_fail "user_hash" "expected 32 lowercase hex chars, got: ${HASH:-<none>}"
	fi

	if [ "$(_jq '.known_clients[0].online | type')" = "boolean" ]; then
		_pass "online is a boolean"
	else
		_fail "online" "expected boolean, got: $(_jq '.known_clients[0].online | type')"
	fi

	# Optional fields are omitted, never emitted empty: a record written
	# before the daemon kept per-peer metadata has no name at all, and a
	# consumer must be able to tell that from "recorded as empty".
	EMPTY_NAMES=$(_jq '[.known_clients[] | select(has("name") and .name == "")] | length')
	if [ "${EMPTY_NAMES:-0}" -eq 0 ]; then
		_pass "no record carries an empty name (absent means unrecorded)"
	else
		_fail "optional fields" "$EMPTY_NAMES record(s) have name == \"\""
	fi

	# first_seen_at and session_count travel together — both come from the same
	# metadata block, so one without the other means a decode bug.
	MISMATCH=$(_jq '[.known_clients[] | select(has("first_seen_at") != has("session_count"))] | length')
	if [ "${MISMATCH:-0}" -eq 0 ]; then
		_pass "first_seen_at and session_count are present or absent together"
	else
		_fail "metadata pairing" "$MISMATCH record(s) have one without the other"
	fi
else
	_skip "record-shape assertions (the store is empty)"
fi

# --- 4b. The store is maintained, not re-read. ---------------------
# It is fetched once and kept current by the refresher, so a connected peer's
# totals move between polls without any refetch. A peer that is NOT connected
# cannot change, so nothing is expected to move for it.
#
# Not compared against /clients: the lifetime totals live on the refresher's
# snapshot but are not part of the /clients JSON, so there is nothing there to
# compare with. Movement over time is the observable that matters anyway.
# Sorted, for the same reason as the row check below: an unsorted page of a
# large store contains no online records at all, and the check would skip
# itself forever while looking like it had run.
_curl "$HOST/api/v0/known_clients?sort=last_seen_at&order=desc&limit=500"
ACTIVE_HASH=$(_jq '[.known_clients[] | select(.online)][0].user_hash')
if [ -n "$ACTIVE_HASH" ] && [ "$ACTIVE_HASH" != "null" ]; then
	BEFORE=$(_jq "[.known_clients[] | select(.user_hash == \"$ACTIVE_HASH\")][0]
		| .downloaded_bytes_total + .uploaded_bytes_total")
	sleep 4
	_curl "$HOST/api/v0/known_clients?sort=last_seen_at&order=desc&limit=500"
	AFTER=$(_jq "[.known_clients[] | select(.user_hash == \"$ACTIVE_HASH\")][0]
		| .downloaded_bytes_total + .uploaded_bytes_total")
	if [ "${AFTER:-0}" -gt "${BEFORE:-0}" ]; then
		_pass "a connected peer's totals move without a refetch ($BEFORE -> $AFTER)"
	else
		# An online but idle peer is legitimately static, and cannot be told
		# apart from a stale cache here. Only a moving one proves the patch.
		_skip "live-total check (the online peer is not transferring)"
	fi
else
	_skip "live-total check (no peer is connected)"
fi

# --- 4c. Every connected peer has a row. ---------------------------
# The store is fetched once, so a peer met afterwards can only be present if
# the refresher added it. Any live peer carrying a user hash must therefore
# appear here -- if it does not, the maintenance has stopped and the endpoint
# is quietly serving a frozen snapshot.
#
# Sorted by last_seen_at descending and read one page: a connected peer is last
# seen *now*, so the online records are the newest in the store and cannot be
# pushed off the first page by anything else. `limit` is clamped to 500 by the
# shared parser, which is well above MaxConnections -- asking for the whole
# store instead would silently return only the first 500 records and look like
# a maintenance failure.
#
# The empty and all-zero hashes are excluded, matching what
# ReconcileKnownClientsLocked() skips: the all-zero one is the placeholder a peer
# reports before sending its real hash, and folding those in would collapse
# every unidentified peer into one fabricated record.
LIVE_CLIENTS_JSON=$(curl -s --max-time 10 "${AUTH[@]}" "$HOST/api/v0/clients?limit=500")
LIVE_TOTAL=$(printf '%s' "$LIVE_CLIENTS_JSON" | jq -r '.clients | length' 2>/dev/null)
LIVE_HASHES=$(printf '%s' "$LIVE_CLIENTS_JSON" \
	| jq -r '.clients[].user_hash // empty | select(. != "" and (test("^0+$") | not))' | sort -u)
if [ -n "$LIVE_HASHES" ]; then
	_curl "$HOST/api/v0/known_clients?sort=last_seen_at&order=desc&limit=500"
	MISSING=0
	CHECKED=0
	for h in $LIVE_HASHES; do
		CHECKED=$((CHECKED+1))
		FOUND=$(_jq "[.known_clients[] | select(.user_hash == \"$h\")] | length")
		[ "${FOUND:-0}" -eq 0 ] && MISSING=$((MISSING+1))
	done
	if [ "$MISSING" -eq 0 ]; then
		_pass "every connected peer has a known-clients row ($CHECKED checked)"
	else
		_fail "maintenance" "$MISSING of $CHECKED connected peer(s) have no row" \
			"the refresher is not folding live peers into the store"
	fi

	# And they are flagged as connected, not merely present.
	ONLINE_N=$(_jq '[.known_clients[] | select(.online)] | length')
	if [ "${ONLINE_N:-0}" -gt 0 ]; then
		_pass "connected peers are flagged online ($ONLINE_N)"
	else
		_fail "online flag" "no record is flagged online while peers are connected"
	fi
elif [ "${LIVE_TOTAL:-0}" -gt 0 ]; then
	# Connected, but nothing to look up: every peer is still on the
	# placeholder hash the filter above drops. Saying "no peer is
	# connected" here would describe the opposite of what happened, and
	# this is exactly the population the filter exists for.
	_skip "per-peer row check ($LIVE_TOTAL peer(s) connected, none identified yet)"
else
	_skip "per-peer row check (no peer is connected)"
fi

# --- 5. Caching. ---------------------------------------------------
_curl "$HOST/api/v0/known_clients?limit=1"
ETAG=$(_header etag)
if [ -n "$ETAG" ]; then
	_pass "response carries an ETag"
	_curl -H "If-None-Match: $ETAG" "$HOST/api/v0/known_clients?limit=1"
	_assert_status 304 "If-None-Match on the same ETag is a 304"
else
	_fail "caching" "no ETag on /known_clients"
fi

# --- 6. HEAD and method rejection. ---------------------------------
_curl -I "$HOST/api/v0/known_clients"
_assert_status 200 "HEAD /known_clients"

for m in POST PUT DELETE PATCH; do
	_curl -X "$m" "$HOST/api/v0/known_clients"
	_assert_status 405 "$m /known_clients is rejected"
done

echo
SKIP_NOTE=""
[ "$SKIP_COUNT" -gt 0 ] && SKIP_NOTE=" ($SKIP_COUNT check(s) skipped)"
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "33-known-clients: all $TEST_COUNT assertions passed$SKIP_NOTE"
	exit 0
fi
echo "33-known-clients: $FAIL_COUNT of $TEST_COUNT assertions FAILED"
exit 1
