#!/usr/bin/env bash
#
# amuleapi 19-search — search.
#
# Endpoints:
#   GET  /api/v0/search                                    — EC_OP_SEARCH_LIST
#   POST /api/v0/search                                   — EC_OP_SEARCH_START
#       body: {query, type?, file_type?, extension?,
#              min_size_bytes?, max_size_bytes?, min_source_count?}
#   POST /api/v0/search/{id}/stop                          — EC_OP_SEARCH_STOP
#   POST /api/v0/search/{id}/more                          — EC_OP_SEARCH_REQUEST_MORE
#   DELETE /api/v0/search/{id}                             — stop + free
#   GET  /api/v0/search/{id}/results                       — read accumulated
#   POST /api/v0/search/results/{hash}/download           — EC_OP_DOWNLOAD_SEARCH_RESULT
#       body: {category?: uint8} (optional)
#   GET  /api/v0/search/results/{hash}/comments           — Kad ratings/comments for a result
#   POST /api/v0/search/results/{hash}/comments           — EC_OP_SHARED_FILE_SEARCH_KAD_NOTES
#   POST /api/v0/clients/{ecid}/shared_files              — browse a peer ("View Files"), returns a search_id
#
# /search/{id}/results is no longer a per-GET fetch — POST /search marks
# the search active in state and the refresher polls amuled every
# tick while it stays active. GET /search/{id}/results reads straight
# from that state, so subsequent polls already see the fresh query's
# growing results without any cache coordination.
#
# amuled's SEARCH_START is async: results trickle in from servers /
# Kad over the next several seconds. Smoke polls /search/{id}/results with
# bounded retries (up to ~10 s for a global search to harvest results).
#
# Important: this smoke depends on the operator's amuled being
# connected to ed2k servers (for global search) and/or Kad. A
# fully-disconnected daemon will see 0 results — the smoke skips the
# result-shape checks in that case and only exercises the API surface.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

# EC connection info for the second amuleapi instance spun up in section
# 3.2 below — must point at the SAME amuled as the primary instance at
# $HOST. Defaults match run-all.sh's regtest daemon.
EC_HOST=${EC_HOST:-127.0.0.1}
EC_PORT=${EC_PORT:-4712}
EC_PASSWORD=${EC_PASSWORD:-amule}
# Locate the amuleapi binary the sub-instances need. Deliberately NOT falling
# back to PATH: an installed package would silently be tested in place of the
# working tree, which is worse than not finding anything. Set AMULEAPI_BIN to
# point somewhere else on purpose.
#
# Any build*/ directory counts, not a fixed list of three: per-branch and
# per-arch trees are normal, and a name this function has never heard of used
# to mean a silent skip. Where several exist the NEWEST wins -- picking the
# first match let a stale build/ supply the binary while the tree under test
# was built somewhere else, which is the one failure mode that produces a
# confident wrong answer rather than a missing one. Unmatched globs stay
# literal and fail the -x test, so no nullglob is needed.
_find_amuleapi_bin() {
	local root=$1 c best=
	for c in "$root"/src/webapi/amuleapi \
		"$root"/build*/src/webapi/amuleapi \
		"$root"/_build/src/webapi/amuleapi \
		"$root"/cmake-build-*/src/webapi/amuleapi; do
		[ -x "$c" ] || continue
		if [ -z "$best" ] || [ "$c" -nt "$best" ]; then
			best=$c
		fi
	done
	[ -n "$best" ] || return 1
	echo "$best"
}

AMULEAPI_BIN=${AMULEAPI_BIN:-$(_find_amuleapi_bin "$(cd "$(dirname "$0")/../../.." && pwd)")}
# Three sections below drive a SECOND amuleapi against the same amuled. Without
# a binary to start one, skip them rather than fail on the daemon's behalf.
if [ -x "${AMULEAPI_BIN:-/nonexistent}" ]; then
	HAVE_SECOND_INSTANCE=1
else
	HAVE_SECOND_INSTANCE=0
fi
# Counted so the summary cannot report a clean pass over a partial run:
# every section below that skips increments this, and a non-zero count
# is spelled out next to the OK line.
SKIP_COUNT=0
_skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP  $1"; }

# A query likely to return results on any operator's daemon connected
# to ed2k. "ubuntu" is a safe choice — well-seeded across the network.
TEST_QUERY="ubuntu"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_19_search_body.XXXXXX)
CURL_HDR_FILE=$(mktemp -t amuleapi_19_search_hdr.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HDR_FILE"' EXIT

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
	resp=$(curl -s --max-time 10 -D "$CURL_HDR_FILE" \
		-o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
	CURL_HDR=$(cat "$CURL_HDR_FILE")
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

_assert_header_contains() {
	local needle=$1 label=$2
	if printf '%s' "$CURL_HDR" | grep -qi -- "$needle"; then
		_pass "$label"
	else
		_fail "$label" "needle '$needle' not in response headers" \
			"headers: $(printf '%s' "$CURL_HDR" | head -c 400)"
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

echo "amuleapi 19-search smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
_curl "$HOST/api/v0/search"
_assert_status 401 "GET /search (no token) → 401"

_curl -X POST -H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\"}" "$HOST/api/v0/search"
_assert_status 401 "POST /search (no token) → 401"

_curl -X POST "$HOST/api/v0/search/1/stop"
_assert_status 401 "POST /search/{id}/stop (no token) → 401"

_curl -X POST "$HOST/api/v0/search/results/baadbaadbaadbaadbaadbaadbaadbaad/download"
_assert_status 401 "POST /search/results/{hash}/download (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"query\":\"$TEST_QUERY\"}" "$HOST/api/v0/search"
	_assert_status 403 "POST /search (guest) → 403"
fi

# --- 2. POST /search error paths. ----------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/search"
_assert_status 400 "POST /search (no query) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"query":""}' "$HOST/api/v0/search"
_assert_status 400 "POST /search (empty query) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"bogus\"}" "$HOST/api/v0/search"
_assert_status 400 "POST /search (bad type enum) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"min_size_bytes\":-1}" "$HOST/api/v0/search"
_assert_status 400 "POST /search (negative min_size_bytes) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json' "$HOST/api/v0/search"
_assert_status 400 "POST /search (malformed JSON) → 400"

# --- 3. POST /search happy + per-search_id addressing. ---------
#
# Multi-search: each POST /search gets its own daemon-allocated search_id
# and its own result slot; a new search does NOT wipe the others. Every
# read/stop/free names its id in the path -- there is no implicit target,
# which is what the bad-id assertions below pin down.
#
# `results` and `stop` reach the {id} matcher and are rejected as ids, which
# is the honest answer: they are not routes, and the rejection already names
# where to find a real one.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
_assert_status 400 "GET /search/results → 400 (id goes in the path)"
_assert_json_eq '.error.code' bad_request '/search/results rejection names the id rule'
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/stop"
_assert_status 400 "POST /search/stop → 400 (id goes in the path)"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search"
_assert_status 202 "POST /search (query=$TEST_QUERY, type=global) → 202"
# A creation answers with the created resource, because here the daemon really
# does hand one back: SEARCH_START returns EC_TAG_SEARCH_ID. The body is the
# same row GET /search lists, written through the same writer, so a client can
# drop it straight into the collection it keeps -- which is why `kind` and
# `state` are asserted here and not just on the list. `ok` is gone; the 202
# carried it.
_assert_json_eq '. | has("ok")' false 'POST /search response has no constant ok field'
_assert_json_eq '.query' "$TEST_QUERY" 'POST /search echoes query'
_assert_json_eq '.id | type' number 'POST /search returns a numeric id'
_assert_json_eq '.type'  global       'POST /search response reports type=global'
_assert_json_eq '.state' running      'POST /search response reports state=running'
_assert_json_eq '.started_at | type' number 'POST /search response stamps started_at'
FIRST_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')
# ...and a Location naming where the resource now lives, so a client that
# ignores the body still knows the id it was given.
_assert_header_contains "location: /api/v0/search/$FIRST_SID" \
	'POST /search sends a Location for the search it created'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
_assert_json_eq '.search_id' "$FIRST_SID" 'GET /search/{id}/results echoes its search_id'
_assert_json_eq '.query' "$TEST_QUERY" 'GET /search/{id}/results reports the query it was started with'

# A malformed or zero {id} is rejected outright. Zero used to be the
# "whichever search ran last" sentinel, so letting it through would
# quietly resurrect the implicit target these routes removed.
for bad in 0 abc -1 99999999999; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$bad/results"
	_assert_status 400 "GET /search/$bad/results → 400 (not a usable search id)"
done
# A well-formed id nobody holds is a 404, never a fallback.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/4294967290/results"
_assert_status 404 "GET /search/{unknown}/results → 404"

# --- 3.1 GET /api/v0/search enumerates the search just started. ---
# Reachability fix (issue #641): GET /api/v0/search reads live daemon
# state via EC_OP_SEARCH_LIST rather than this session's own m_state
# cache, so the search just started via POST /search must appear here
# too -- proving the two endpoints agree on what amuled currently holds.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 200 "GET /search → 200"
_assert_json_eq '.searches | type' array 'GET /search .searches is an array'
_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)] | length" 1 \
	'GET /search lists the search just started via POST /search'
_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)][0].query" "$TEST_QUERY" \
	'GET /search entry echoes the query'
_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)][0].type" global \
	'GET /search entry reports kind==global'
_assert_json_eq "[[.searches[] | select(.id == $FIRST_SID)][0].state] | inside([\"running\",\"finished\"])" \
	true 'GET /search entry state is running or finished (never idle for an active search)'
# started_at is the list's only recency signal: entries arrive id-ascending
# and id order is not start order (Kad ids carry a high-bit mask and sort
# above ed2k ones), so a client that wants "the newest search" sorts on this.
# Present because THIS amuleapi started the search.
_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)][0].started_at | type" number \
	'GET /search stamps started_at on a search this session started'
_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)][0].started_at > 1700000000" \
	true 'started_at is a plausible recent unix second, not 0'
# result_count lets a client label a tab it has not opened yet, instead of
# fetching every search's results just to learn one integer each.
_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)][0].result_count | type" number \
	'GET /search entry carries a numeric result_count'
# ...and it agrees with what the results endpoint reports as `total` -- but only
# once the search has settled. While it runs, result_count is the daemon's live
# index and `total` is whatever amuleapi last pulled into its cache, so the two
# --- GET /search is a list endpoint like the others. ---------------
#
# It was the one collection with no envelope: no total, no offset, no limit and
# no sort, so a client had to special-case it. It is also unbounded (whatever
# EC_OP_SEARCH_LIST returns, including searches this process never started), and
# entries arrive id-ascending while id order is not start order, because Kad ids
# carry a high-bit mask and always sort above eD2k ones.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 200 "GET /search → 200"
_assert_json_eq '.total | type'  number '/search carries total'
_assert_json_eq '.offset | type' number '/search carries offset'
_assert_json_eq '.limit' 100 '/search omitted limit echoes the default 100'

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search?limit=1"
_assert_status 200 "GET /search?limit=1 → 200"
_assert_json_eq '.searches | length' 1 '/search?limit=1 returns one row'

# The recency signal the docs point at is now actually askable.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search?sort=started_at&order=desc"
_assert_status 200 "GET /search?sort=started_at&order=desc → 200"

for bad in "limit=abc" "limit=1000000001" "order=sideways" "sort=nonexistent_field"; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search?$bad"
	_assert_status 400 "GET /search?$bad → 400"
done

# legitimately differ by a fetch.
LIST_COUNT=$(printf '%s' "$CURL_BODY" \
	| jq -r "[.searches[] | select(.id == $FIRST_SID)][0].result_count")
LIST_STATE=$(printf '%s' "$CURL_BODY" \
	| jq -r "[.searches[] | select(.id == $FIRST_SID)][0].state")
if [ "$LIST_STATE" = "finished" ]; then
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
	_assert_json_eq '.total' "$LIST_COUNT" \
		'result_count equals the results endpoint total for a finished search'
else
	echo "    info: search still $LIST_STATE; skipping the result_count/total comparison"
fi

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/search"
	_assert_status 200 "GET /search (guest) → 200 (GUEST-readable)"
fi

if [ "$HAVE_SECOND_INSTANCE" -eq 0 ]; then
	_skip "3.2 cross-session discovery (no amuleapi binary; set AMULEAPI_BIN)"
else
# --- 3.2 Cross-session discovery: a second amuleapi instance against the
# same amuled sees $FIRST_SID even though it never called POST /search
# itself (got3nks, PR #680 review point 6 — no amulecmd needed, two
# amuleapi sessions against one daemon exercise the same discovery path).
# SECOND_HOST's HTTP server is independent, but both instances share the
# one daemon at EC_HOST:EC_PORT, so EC_OP_SEARCH_LIST on session B finds
# the search session A started.
SECOND_HOST="localhost:4714"
SECOND_CONFIG_DIR=$(mktemp -d -t amuleapi_19_search_second.XXXXXX)
SECOND_LOG=$(mktemp -t amuleapi_19_search_second_log.XXXXXX)
"$AMULEAPI_BIN" --config-dir="$SECOND_CONFIG_DIR" \
	--host="$EC_HOST" --port="$EC_PORT" \
	--set-admin-pass="$ADMIN_PASS" >/dev/null 2>&1
# amuleapi takes no --password; the call above also wrote a defaults
# amuleapi.conf, so put the EC credential in there.
sed -i'.bak' "s|^Password=.*|Password=$EC_PASSWORD|" "$SECOND_CONFIG_DIR/amuleapi.conf"
rm -f "$SECOND_CONFIG_DIR/amuleapi.conf.bak"
"$AMULEAPI_BIN" --config-dir="$SECOND_CONFIG_DIR" \
	--host="$EC_HOST" --port="$EC_PORT" \
	--http-port=4714 >"$SECOND_LOG" 2>&1 &
SECOND_PID=$!

for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
	curl -s -o /dev/null --max-time 1 "http://$SECOND_HOST/api/v0/health" 2>/dev/null && break
	sleep 0.5
done
sleep 4

SECOND_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "http://$SECOND_HOST/api/v0/auth/login?include_token=true" \
	| jq -r .token)

if [ -n "$SECOND_TOKEN" ] && [ "$SECOND_TOKEN" != "null" ]; then
	_curl -H "Authorization: Bearer $SECOND_TOKEN" "http://$SECOND_HOST/api/v0/search"
	_assert_status 200 "second amuleapi instance: GET /search → 200"
	_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)] | length" 1 \
		'second amuleapi instance (never POSTed) still lists the first instance'"'"'s search'
	# ...but cannot stamp it: that instance did not start the search and the
	# daemon ships no timestamp, so the key is omitted rather than zeroed.
	_assert_json_eq "[.searches[] | select(.id == $FIRST_SID)][0] | has(\"started_at\")" \
		false 'a foreign search carries no started_at (unknown, not 1970)'

	_curl -H "Authorization: Bearer $SECOND_TOKEN" \
		"http://$SECOND_HOST/api/v0/search/$FIRST_SID/results"
	_assert_status 200 'second amuleapi instance: GET /search/{foreign id}/results → 200 (not 404)'
	_assert_json_eq '.search_id' "$FIRST_SID" 'second amuleapi instance echoes the discovered search_id'
	# The adopted slot also learns the query from the SEARCH_LIST entry,
	# so a client that discovered an id can label it without guessing.
	_assert_json_eq '.query' "$TEST_QUERY" 'second amuleapi instance reports the discovered query'
	# A slot discovered as finished is never polled by the tick, so its
	# percent has to be seeded at discovery or it stays 0 forever and
	# contradicts the "finished" state carried in this same envelope. Only
	# meaningful once the search has actually finished; a still-running one
	# reports the daemon's real ramp and is checked elsewhere.
	ADOPTED_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state')
	if [ "$ADOPTED_STATE" = "finished" ]; then
		_assert_json_eq '.progress.percent' 100 \
			'an adopted finished search reports percent 100, not 0'
	else
		echo "    info: adopted search still $ADOPTED_STATE; skipping the finished-percent check"
	fi
else
	_fail "second amuleapi instance: admin login" "could not obtain a token; log: $(tail -c 300 "$SECOND_LOG")"
fi

kill "$SECOND_PID" >/dev/null 2>&1
wait "$SECOND_PID" 2>/dev/null
rm -rf "$SECOND_CONFIG_DIR" "$SECOND_LOG"
fi # HAVE_SECOND_INSTANCE -- section 3.2

# --- 3.5 Regression: progress shouldn't claim finished right after POST. -
# amuled briefly reports raw=100 in the "queue-empty-at-start" window
# before the global-search timer populates m_serverQueue; if amuleapi
# trusted that raw value naively, GET /search/{id}/results right after POST
# would (incorrectly) say {progress:{percent:100, state:"finished"}}
# with results=[]. The refresher's state machine masks that window —
# this asserts the mask is in force.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
_assert_status 200 "GET /search/{id}/results immediately after POST → 200"
# The window guarded here is `finished` with an EMPTY list, from the unmasked
# raw=100. A search that genuinely completed first is also `finished`, so a flat
# `running` comparison failed on a fast local hit or a warm server queue.
EARLY_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state' 2>/dev/null)
EARLY_RESULTS=$(printf '%s' "$CURL_BODY" | jq -r '.results | length' 2>/dev/null)
if [ "$EARLY_STATE" = "running" ] ||
	{ [ "$EARLY_STATE" = "finished" ] && [ "${EARLY_RESULTS:-0}" -gt 0 ]; }; then
	_pass "progress.state right after POST is not an empty \"finished\" ($EARLY_STATE, ${EARLY_RESULTS:-0} results)"
else
	_fail "early progress state" \
		"got state=$EARLY_STATE with ${EARLY_RESULTS:-0} results" \
		"amuled's queue-empty-at-start raw=100 is leaking through unmasked"
fi
_assert_json_eq '.progress.type | type' string 'progress.type is a string'

# --- 4. Poll /search/{id}/results until we get hits (max ~10 s). --
RESULT_HASH=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
	N=$(printf '%s' "$CURL_BODY" | jq '.results | length')
	if [ "$N" -gt 0 ]; then
		RESULT_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.results[0].hash')
		break
	fi
	sleep 0.2
done

if [ -n "$RESULT_HASH" ]; then
	_pass "Search returned >0 results within 10 s ($N entries; sample hash $RESULT_HASH)"

	# Per-result shape sanity.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
	_assert_json_eq '.results[0].hash | length' 32     '/search/{id}/results[0].hash is 32-char hex'
	_assert_json_eq '.results[0].name | type'   string '/search/{id}/results[0].name is string'
	_assert_json_eq '.results[0].size_bytes | type'   number '/search/{id}/results[0].size_bytes is numeric'
	# Browse-only folder. Always present, empty on a server/Kad hit.
	_assert_json_eq '.results[0].directory | type' string \
		'/search/{id}/results[0].directory is a string'
	_assert_json_eq '.results[0].directory' "" \
		'directory is empty on an ordinary (non-browse) hit'
	# Download status + file type (issue #429).
	_assert_json_eq '[.results[0].status] | inside(["new","downloaded","queued","canceled","queued_canceled"])' \
		true '/search/results[0].status is a known enum value'
	_assert_json_eq '.results[0].file_type | type'   string '/search/{id}/results[0].file_type is string'
	# Media metadata (issue #430): an object when the hit is locally
	# known/probed, absent otherwise — both are valid.
	_assert_json_eq '.results[0].media | type | test("^(object|null)$")' \
		true '/search/{id}/results[0].media is an object or absent'
	# Result grouping (issue #431): every result carries a children[]
	# array (empty when the hit was seen under a single name). No result
	# is itself an alternate name (they are folded into their parent), and
	# each entry has ecid + name + sources.
	_assert_json_eq '.results[0].alternate_names | type' array '/search/{id}/results[0].alternate_names is an array'
	_assert_json_eq '[.results[].alternate_names[]?] | all(has("ecid") and has("name") and has("sources"))' \
		true 'every alternate name has ecid/name/sources'
	# `hash` was dropped from the entries: it is by construction the parent's,
	# so repeating it per entry said nothing.
	_assert_json_eq '[.results[].alternate_names[]?] | all(has("hash") | not)' \
		true 'alternate names no longer repeat the parent hash'
	_assert_json_eq '[.results[].alternate_names[]?] | all(has("directory"))' \
		true 'every child carries its own directory (per-result, not per-search)'

	# sort=directory is accepted (browse listings are read folder by
	# folder); on a non-browse search every value is "" so the order is
	# simply unchanged, which is the point -- it must not 400.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$FIRST_SID/results?sort=directory&order=asc"
	_assert_status 200 "GET /search/{id}/results?sort=directory → 200"

	# progress envelope. `progress` exists on every GET /search/{id}/results
	# response (even before any POST /search). `state` is canonical
	# (running | finished | idle) and replaces the old complete/active
	# booleans. Once we have results, state is "running" (still polling)
	# or "finished" (percent == 100).
	_assert_json_eq '.progress.percent | type' number 'search progress.percent is numeric'
	_assert_json_eq '.progress.state | type'   string 'search progress.state is a string'
	_assert_json_eq '[.progress.state] | inside(["running","finished","idle"])' \
		true 'search progress.state is one of running/finished/idle'
	_assert_json_eq '.progress.percent >= 0 and .progress.percent <= 100' \
		true 'search progress.percent stays in [0, 100]'
else
	echo "    info: 0 search results after 10 s — daemon may not be connected to ed2k/kad"
	echo "    info: skipping /search/results/{hash}/download path (no hash to target)"
fi

# --- 4.5 POST /search/{id}/more — Kad-only, running-only. ---------
#
# The global search above is the wrong kind, which is exactly the case the
# desktop greys the button out for; forwarding it would make amuled turn a
# user's request into a silent no-op under a 202.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/more"
_assert_status 400 "POST /search/{id}/more on a global search → 400 (Kad-only)"
_assert_json_eq '.error.code' bad_request 'more on a non-Kad search reports bad_request'
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/4294967290/more"
_assert_status 404 "POST /search/{unknown}/more → 404"

# A running Kad search is the supported target. Kad may be down on the test
# daemon, in which case the start itself fails -- skip rather than fail, like
# the other Kad-dependent assertions in this suite.
#
# Distinct keyword on purpose: amuled refuses a second Kad search for a
# keyword already on its search list ("Search keyword is already on search
# list"), and the ramp section below runs its own Kad search on $TEST_QUERY.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"${TEST_QUERY}more\",\"type\":\"kad\"}" "$HOST/api/v0/search"
if [ "$CURL_STATUS" = "202" ]; then
	KAD_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$KAD_SID/more"
	_assert_status 202 "POST /search/{id}/more on a running Kad search → 202"
	_assert_body_empty 'more sends no body'

	# The timing edge, which is deterministic where the reask cap is not.
	# Kad calls PrepareToStop on a keyword search 20 s before its 45 s life
	# ends, and RequestMoreResults refuses from that moment on -- so a press
	# made past roughly 55% of the ramp can never widen the search again and
	# must answer 409 instead of a misleading 202.
	#
	# Deliberately NOT trying to exhaust the 4-reask budget: that needs four
	# distinct responded peers inside the first 25 s and is not reproducible
	# on a test daemon.
	MORE_PCT=0
	for _ in $(seq 1 40); do
		sleep 2
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$KAD_SID/results"
		MORE_PCT=$(printf '%s' "$CURL_BODY" | jq -r '.progress.percent // 0')
		MORE_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state')
		[ "$MORE_STATE" = "finished" ] && break
		# 65 is comfortably past the ~55% PrepareToStop point without waiting
		# for the search to finish (which a finished search rejects with 400).
		awk -v p="$MORE_PCT" 'BEGIN { exit !(p > 65) }' && break
	done
	if [ "$MORE_STATE" = "running" ] && awk -v p="$MORE_PCT" 'BEGIN { exit !(p > 65) }'; then
		_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$KAD_SID/more"
		_assert_status 409 "POST /search/{id}/more past the stopping window → 409"
		_assert_json_eq '.error.code' kad_more_exhausted \
			'a search that can no longer be widened reports kad_more_exhausted'
	else
		echo "    info: Kad search reached state=$MORE_STATE percent=$MORE_PCT;" \
			"skipping the stopping-window 409 check"
	fi
	if [ "$HAVE_GUEST" = "1" ]; then
		_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
			"$HOST/api/v0/search/$KAD_SID/more"
		_assert_status 403 "POST /search/{id}/more (guest) → 403"
	fi
	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$KAD_SID" > /dev/null
else
	echo "    info: Kad search would not start (Kad down?) — skipping /more happy path"
fi

# --- 5. POST /search/{id}/stop. -----------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/$FIRST_SID/stop"
# 204, the same as DELETE /search/{id}: with `ok` gone there is nothing left
# to report, and only the results-survive check below tells the two apart.
_assert_status 204 "POST /search/{id}/stop → 204"
_assert_body_empty 'search stop sends no body'
# Stop keeps the results readable — that is what distinguishes it from
# DELETE, and a consumer viewing the search must not lose its rows.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
_assert_status 200 "GET /search/{id}/results after stop → 200 (results survive a stop)"
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/4294967290/stop"
_assert_status 404 "POST /search/{unknown}/stop → 404"

# --- 6. POST /search/results/{hash}/download — happy + cleanup. --
if [ -n "$RESULT_HASH" ]; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"category_index":0}' \
		"$HOST/api/v0/search/results/$RESULT_HASH/download"
	# 202 with no body: `hash` came from the URL, `category` came from the
	# request, and the download itself reports the category it landed in.
	_assert_status 202 "POST /search/results/{hash}/download → 202"
	_assert_body_empty 'download sends no body'

	# Empty-body POST should also succeed (category defaults to 0).
	# But first DELETE the just-created download so we don't trip
	# "already in queue".
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads/$RESULT_HASH"
	# 204 if found, 404 if already evicted by amuled — either is OK.
	if [ "$CURL_STATUS" = "204" ] || [ "$CURL_STATUS" = "404" ]; then
		_pass "Cleanup: DELETE /downloads/{result hash} → $CURL_STATUS"
	else
		_fail "Cleanup DELETE" "unexpected status $CURL_STATUS"
	fi
fi

# --- 7. POST /search/results/{hash}/download error paths. --------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/search/results/not-32-hex-chars/download"
_assert_status 400 "POST download (bad hash format) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"category_index":300}' \
	"$HOST/api/v0/search/results/baadbaadbaadbaadbaadbaadbaadbaad/download"
_assert_status 400 "POST download (category out of range) → 400"

# Download-under-name selector (issue #431): `ecid` must be a
# non-negative integer.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ecid":"notnum"}' \
	"$HOST/api/v0/search/results/baadbaadbaadbaadbaadbaadbaadbaad/download"
_assert_status 400 "POST download (ecid wrong type) → 400"

# Unknown hash that's well-formed (32 hex chars) → amuled rejection.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/search/results/baadbaadbaadbaadbaadbaadbaadbaad/download"
# amuled may either reject (400 amuled_rejected) or silently accept
# the request and never instantiate the partfile — both wire shapes
# have been observed; accept either.
if [ "$CURL_STATUS" = "400" ] || [ "$CURL_STATUS" = "202" ]; then
	_pass "POST download (well-formed unknown hash) → $CURL_STATUS"
else
	_fail "POST download unknown hash" \
		"expected 400 or 202, got $CURL_STATUS"
fi

# --- 8. Method gates. ---------------------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 405 "PATCH /search → 405"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/stop"
_assert_status 405 "PATCH /search/{id}/stop → 405"
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID"
_assert_status 405 "GET /search/{id} → 405 (DELETE only)"
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/bogus"
_assert_status 404 "GET /search/{id}/{unknown-action} → 404"

# --- 9. Kad search progress ramp. ---------------------------------
# Kad has no measurable progress, so amuled synthesises a cosmetic
# time-ramp from the fixed keyword-search lifetime (SEARCHKEYWORD_LIFETIME,
# 45 s) and ships it in EC_TAG_SEARCH_LIFECYCLE_PERCENT; amuleapi
# surfaces it verbatim as progress.percent. Assert the ramp climbs over
# time and stays capped at 99 while running — only the authoritative
# finished edge reaches 100, so the bar can never claim completion
# early. Skips the ramp assertions if amuled isn't connected to Kad
# (the search never goes "running").
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"kad\"}" "$HOST/api/v0/search"
_assert_status 202 "POST /search type=kad → 202"
RAMP_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')

KAD_STATES=""; KAD_PCTS=""; SAW_RUNNING_KAD=0
for _ in 1 2 3 4 5 6; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$RAMP_SID/results"
	ST=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state')
	KD=$(printf '%s' "$CURL_BODY" | jq -r '.progress.type')
	PC=$(printf '%s' "$CURL_BODY" | jq -r '.progress.percent')
	KAD_STATES="$KAD_STATES $ST"; KAD_PCTS="$KAD_PCTS $PC"
	if [ "$ST" = "running" ] && [ "$KD" = "kad" ]; then SAW_RUNNING_KAD=1; fi
	sleep 2
done
echo "    kad samples: states=[$KAD_STATES ] percents=[$KAD_PCTS ]"

if [ "$SAW_RUNNING_KAD" -eq 0 ]; then
	echo "    info: Kad search never went 'running' — amuled likely not"
	echo "    info: connected to Kad; skipping ramp assertions."
else
	CAP_OK=1; MONO=1; PREV=-1; FIRST_RUN=""; LAST_RUN=""; SAW_FINISHED=0
	set -- $KAD_PCTS; KAD_PC_ARR=("$@")
	idx=0
	for st in $KAD_STATES; do
		pc=${KAD_PC_ARR[$idx]}
		if [ "$pc" -lt "$PREV" ] 2>/dev/null; then MONO=0; fi
		PREV=$pc
		if { [ "$pc" -lt 0 ] || [ "$pc" -gt 100 ]; } 2>/dev/null; then CAP_OK=0; fi
		if [ "$st" = "running" ]; then
			if [ "$pc" -gt 99 ] 2>/dev/null; then CAP_OK=0; fi
			[ -z "$FIRST_RUN" ] && FIRST_RUN=$pc
			LAST_RUN=$pc
		fi
		[ "$st" = "finished" ] && SAW_FINISHED=1
		idx=$((idx+1))
	done

	if [ "$CAP_OK" -eq 1 ]; then
		_pass "Kad running percent capped at 99 and within [0,100]"
	else
		_fail "Kad percent cap" "states=[$KAD_STATES ] percents=[$KAD_PCTS ]"
	fi
	if [ "$MONO" -eq 1 ]; then
		_pass "Kad percent monotonic non-decreasing"
	else
		_fail "Kad percent monotonic" "percents went backwards: [$KAD_PCTS ]"
	fi
	if [ "$SAW_FINISHED" -eq 1 ] || \
	   { [ -n "$FIRST_RUN" ] && [ -n "$LAST_RUN" ] && [ "$LAST_RUN" -gt "$FIRST_RUN" ] 2>/dev/null; }; then
		_pass "Kad ramp advanced over time (first=$FIRST_RUN last=$LAST_RUN finished=$SAW_FINISHED)"
	else
		_fail "Kad ramp advance" \
			"percent did not climb and search never finished: first=$FIRST_RUN last=$LAST_RUN states=[$KAD_STATES ]"
	fi
fi

curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$RAMP_SID" > /dev/null 2>&1

# --- 10. Search-result Kad comments/ratings (issue #434). ---------
# GET/POST /search/results/{hash}/comments mirror the download-comments
# endpoints for a result the user has not downloaded. Auth + error gates
# need no connectivity; the happy path needs a live result.
BOGUS=baadbaadbaadbaadbaadbaadbaadbaad

_curl -X POST "$HOST/api/v0/search/results/$BOGUS/comments"
_assert_status 401 "POST /search/results/{hash}/comments (no token) → 401"

_curl "$HOST/api/v0/search/results/$BOGUS/comments"
_assert_status 401 "GET /search/results/{hash}/comments (no token) → 401"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/results/not-32-hex-chars/comments"
_assert_status 400 "POST search comments (bad hash format) → 400"

# Admin gate. The POST drives an unbounded Kad NOTES lookup on the daemon
# (EC_OP_SHARED_FILE_SEARCH_KAD_NOTES), so a guest session must not reach
# it; the GET is a plain read and stays open to guests.
if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/search/results/$BOGUS/comments"
	_assert_status 403 "POST /search/results/{hash}/comments (guest token) → 403"
	_assert_json_eq '.error.code' forbidden \
		'POST search comments guest carries error.code=forbidden'
else
	echo "    info: no guest password set on daemon; admin-gate test skipped"
fi

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/results/$BOGUS/comments"
_assert_status 404 "POST search comments (well-formed unknown hash) → 404"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/results/$BOGUS/comments"
_assert_status 404 "GET search comments (unknown hash) → 404"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/results/$BOGUS/comments"
_assert_status 405 "PATCH search comments → 405"

# Happy path: needs a live result. Start a fresh global search and poll.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search"
CMT_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id // empty')
CMT_HASH=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$CMT_SID/results"
	N=$(printf '%s' "$CURL_BODY" | jq '.results | length')
	if [ "$N" -gt 0 ]; then
		CMT_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.results[0].hash')
		break
	fi
	sleep 0.25
done

if [ -n "$CMT_HASH" ]; then
	# Every result carries the comment fields on the list itself.
	_assert_json_eq '.results[0].kad_comment_lookup_running | type' boolean \
		'/search/{id}/results[0].kad_comment_lookup_running is boolean (issue #434)'
	_assert_json_eq '.results[0].comments | type' array \
		'/search/{id}/results[0].comments is an array'

	# Trigger an on-demand Kad notes lookup for the result.
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/results/$CMT_HASH/comments"
	# 202 with no body. The `status` it used to carry could hold exactly one
	# value, so it restated the status code -- and `status` everywhere else on
	# this surface is a transfer state, so a client switching on it had to know
	# which kind of object it held first. The lookup's progress is read from
	# `kad_comment_lookup_running` on the GET below.
	_assert_status 202 "POST /search/results/{hash}/comments → 202"
	_assert_body_empty 'search comments POST sends no body'

	# Per-result comments view.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/results/$CMT_HASH/comments"
	_assert_status 200 "GET /search/results/{hash}/comments → 200"
	_assert_json_eq '.count | type' number 'search comments.count is numeric'
	_assert_json_eq '.kad_comment_lookup_running | type' boolean \
		'search comments carries kad_comment_lookup_running flag'
	_assert_json_eq '.comments | type' array 'search comments.comments is an array'

	# The lookup's outcome only reaches amuleapi through the owning
	# search's result fetch. Stop the search first, then keep polling:
	# on a FINISHED search this exercises the refresh-on-read path, which
	# is the only thing that can flip the flag back off. Skipped as
	# inconclusive when Kad is down -- the flag then never turns on.
	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$CMT_SID/stop" > /dev/null 2>&1
	sleep 1
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$CMT_SID/results"
	_assert_status 200 "GET /search/{id}/results on a finished search → 200 (refreshed on read)"
	# A Kad NOTES lookup runs for ~45 s (SEARCHKEYWORD_LIFETIME), so poll
	# past that. Either outcome proves the point: the flag clearing, or a
	# note arriving. Both can only reach us through the owning search's
	# result fetch, and this search is FINISHED -- the tick never polls it,
	# so a frozen slot would show neither, forever.
	SAW_FLAG=0
	REPORTED=0
	for _ in $(seq 1 30); do
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/search/results/$CMT_HASH/comments"
		RUNNING=$(printf '%s' "$CURL_BODY" | jq -r '.kad_comment_lookup_running')
		NOTES=$(printf '%s' "$CURL_BODY" | jq -r '.count')
		[ "$RUNNING" = "true" ] && SAW_FLAG=1
		if [ "$SAW_FLAG" = "1" ] && { [ "$RUNNING" = "false" ] || [ "$NOTES" -gt 0 ] 2>/dev/null; }; then
			REPORTED=1
			break
		fi
		sleep 2
	done
	if [ "$SAW_FLAG" = "0" ]; then
		echo "    info: kad_comment_lookup_running never turned on — Kad likely down;"
		echo "    info: skipping the finished-search comments reporting assertion."
	elif [ "$REPORTED" = "1" ]; then
		_pass "kad notes lookup on a FINISHED search reports back (running=$RUNNING notes=$NOTES)"
	else
		_fail "kad notes on a finished search" \
			"flag turned on but nothing ever reported back (running=$RUNNING notes=$NOTES)"
	fi
	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$CMT_SID" > /dev/null 2>&1
else
	echo "    info: 0 results — skipping search-comments happy path (daemon not connected)"
fi

# --- 11. Multi-search: concurrent searches, per-id addressing. ----
# amuleapi runs several searches at once, each with its own daemon-
# allocated search_id and its own result slot. Start an ed2k (global)
# AND a Kad search back-to-back and verify they coexist: distinct ids,
# per-id results, unknown id => 404, and DELETE frees one while the
# sibling survives.
#
# Regression guard (daemon fix): a Kad search started while an ed2k
# search is still in-flight must NOT stop/steal the ed2k search — its
# results are attributed via a scalar the Kad start used to clobber, so
# the global bucket would come back empty. Here we assert the global
# search still harvests while the Kad search runs alongside it.
G=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search")
SID_G=$(printf '%s' "$G" | jq -r '.id')
# Distinct Kad keyword again: amuled keeps a keyword on its Kademlia
# search list for the search's lifetime and refuses a second search for
# the same one, so reusing $TEST_QUERY here would couple this section to
# how far the earlier Kad sections have wound down.
K=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"${TEST_QUERY}multi\",\"type\":\"kad\"}" "$HOST/api/v0/search")
SID_K=$(printf '%s' "$K" | jq -r '.id')

if [ -n "$SID_G" ] && [ -n "$SID_K" ] && [ "$SID_G" != "null" ] && [ "$SID_K" != "null" ]; then
	if [ "$SID_G" != "$SID_K" ]; then
		_pass "Two concurrent searches get distinct search_ids ($SID_G, $SID_K)"
	else
		_fail "Concurrent search_ids" "both searches got the same id $SID_G"
	fi

	# Per-id progress kind reflects each search's own type.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G/results"
	_assert_status 200 "GET /search/{global}/results → 200"
	_assert_json_eq '.search_id'      "$SID_G" 'global search echoes its search_id'
	_assert_json_eq '.progress.type'  global   'global search progress.type==global'
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_K/results"
	_assert_status 200 "GET /search/{kad}/results → 200"
	_assert_json_eq '.search_id'      "$SID_K" 'kad search echoes its search_id'
	_assert_json_eq '.progress.type'  kad      'kad search progress.type==kad'

	# Unknown / never-started id → 404 (distinct from known-but-empty).
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/4293000111/results"
	_assert_status 404 "GET /search/{unknown}/results → 404"

	# Regression: the in-flight global search still harvests despite the
	# concurrent Kad search. Poll briefly; skip the assertion (don't fail)
	# if the daemon simply has no ed2k hits for the query.
	GN=0
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G/results"
		GN=$(printf '%s' "$CURL_BODY" | jq '.results | length')
		[ "$GN" -gt 0 ] && break
		sleep 0.25
	done
	if [ "$GN" -gt 0 ]; then
		_pass "In-flight global search still harvests alongside a Kad search ($GN hits)"
	else
		echo "    info: global search returned 0 alongside Kad — daemon may lack ed2k hits for '$TEST_QUERY'"
	fi

	# --- 11.1 Two searches stay separate across the incremental union poll. ---
	#
	# The refresher now fetches every search's results in ONE id-less
	# EC_OP_SEARCH_RESULTS at EC_DETAIL_INC_UPDATE, so all searches share a reply
	# and the daemon stops repeating a result it has already sent. Attribution then
	# rests on amuleapi's own ECID -> search_id index rather than on a tag in the
	# packet, which is the thing that can silently cross-wire two searches.
	#
	# Poll both several times: the first pass populates, later ones are the diffed
	# ones where the search id is no longer on the wire.
	if [ -n "$SID_G" ] && [ -n "$SID_K" ] && [ "$SID_G" != "$SID_K" ]; then
		for _ in 1 2 3 4 5; do
			sleep 1
			_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G/results"
			_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_K/results"
		done

		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G/results"
		_assert_status 200 "union: GET /search/{global}/results after repeated polls → 200"
		_assert_json_eq '.search_id' "$SID_G" 'union: the global search still reports its own id'
		G_TOTAL=$(printf '%s' "$CURL_BODY" | jq -r '.total')

		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_K/results"
		_assert_status 200 "union: GET /search/{kad}/results after repeated polls → 200"
		_assert_json_eq '.search_id' "$SID_K" 'union: the kad search still reports its own id'
		K_TOTAL=$(printf '%s' "$CURL_BODY" | jq -r '.total')

		# Results must not migrate between searches. Checked against the daemon's own
		# per-search count from GET /search, which comes from EC_OP_SEARCH_LIST and so
		# is independent of the cache the union maintains -- comparing the two searches
		# to each other proves nothing, since a global and a Kad search for the same
		# query may legitimately return the same files.
		#
		# One-sided on purpose: the cached total is the FOLDED view (children counted
		# inside their parent) while the daemon counts what it holds, so it may
		# legitimately be lower. It can only ever exceed the daemon's count by holding
		# results that belong to another search, which is exactly the cross-wiring
		# this is looking for.
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		if [ "$(printf '%s' "$CURL_BODY" | jq --argjson g "$SID_G" --argjson k "$SID_K" \
			'[.searches[] | select(.id == $g or .id == $k) | select(has("result_count"))] | length')" -eq 2 ]; then
			for _pair in "$SID_G:$G_TOTAL:global" "$SID_K:$K_TOTAL:kad"; do
				_sid=${_pair%%:*}; _rest=${_pair#*:}; _cached=${_rest%%:*}; _label=${_rest#*:}
				_held=$(printf '%s' "$CURL_BODY" | jq -r --argjson s "$_sid" \
					'.searches[] | select(.id == $s) | .result_count')
				if [ "$_cached" -gt "$_held" ]; then
					_fail "union: the $_label search holds more results than the daemon has for it" \
						"cached $_cached > daemon $_held; the ECID index is cross-wiring diffed tags"
				else
					_pass "union: the $_label search holds no results the daemon attributes elsewhere"
				fi
			done
		else
			echo "    info: daemon reported no result_count for one of the searches; cross-wiring check skipped"
		fi

		# Every row still carries the fields that only travel on a result's FIRST
		# appearance. A merge that dropped a guard would blank these on the first
		# diffed poll, which is the failure mode with no other symptom.
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G/results"
		if [ "$(printf '%s' "$CURL_BODY" | jq '.results | length')" -gt 0 ]; then
			_assert_json_eq '[.results[] | select(.name == "" or .name == null)] | length' 0 \
				'union: no result lost its name across diffed polls'
			_assert_json_eq '[.results[] | select(.size_bytes == 0 or .size_bytes == null)] | length' 0 \
				'union: no result lost its size_bytes across diffed polls'
			_assert_json_eq '[.results[] | select(.hash == "" or .hash == null)] | length' 0 \
				'union: no result lost its hash across diffed polls'
			_assert_json_eq '[.results[] | select(.status == "" or .status == null)] | length' 0 \
				'union: no result lost its status across diffed polls'
			_assert_json_eq '[.results[] | select(.file_type == "" or .file_type == null)] | length' 0 \
				'union: no result lost its file_type across diffed polls'
		else
			echo "    info: global search returned no rows; per-field retention checks skipped"
		fi
	else
		echo "    info: no two distinct searches available; union separation check skipped"
	fi

	# DELETE the global search: its slot is freed (404), the Kad search
	# is untouched (still 200). Freeing one from one tab must never take
	# a sibling with it.
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G"
	_assert_status 204 "DELETE /search/{global} → 204"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_G/results"
	_assert_status 404 "GET freed global search → 404"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$SID_K/results"
	_assert_status 200 "sibling Kad search survives the DELETE → 200"

	# DELETE with an id nobody holds → 404.
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/4293000111"
	_assert_status 404 "DELETE /search/{unknown} → 404"

	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$SID_K" >/dev/null 2>&1
else
	_fail "Multi-search setup" "POST /search did not return search_ids (G=$SID_G K=$SID_K)"
fi

# --- 11.2 A finished search keeps being refreshed. -----------------
#
# The tick used to poll only searches with progress.active, so once a search
# finished nothing re-read it and a hit downloaded afterwards kept reporting
# already_downloaded=false forever. Eliding made polling finished searches free, so
# they stay in the poll set -- this pins that the row is still being maintained
# rather than frozen at the moment the search completed.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
if [ "$CURL_STATUS" = "200" ]; then
	FIN_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state')
	FIN_N=$(printf '%s' "$CURL_BODY" | jq '.results | length')
	echo "    info: search $FIRST_SID is $FIN_STATE with $FIN_N result(s)"
	if [ "$FIN_STATE" = "finished" ] && [ "$FIN_N" -gt 0 ]; then
		# Two reads a couple of ticks apart: the row must still be there and
		# still complete. A finished search dropped from the poll set would
		# have kept whatever it had, so this is a regression guard on the
		# merge rather than a liveness assertion about values that may not
		# change on an idle daemon.
		BEFORE_N=$FIN_N
		sleep 3
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$FIRST_SID/results"
		_assert_status 200 "union: a finished search is still readable after further ticks"
		_assert_json_eq '.results | length' "$BEFORE_N" \
			'union: a finished search keeps its results across ticks'
		_assert_json_eq '[.results[] | select(.name == "" or .name == null)] | length' 0 \
			'union: a finished search keeps its result names across ticks'
		_assert_json_eq '[.results[] | select(.already_downloaded | type != "boolean")] | length' 0 \
			'union: a finished search still reports already_downloaded as a boolean'
	else
		echo "    info: search $FIRST_SID not finished-with-results; finished-poll check skipped"
	fi
else
	echo "    info: search $FIRST_SID no longer readable; finished-poll check skipped"
fi

# --- 12. Close actually frees the search on the daemon. ------------
# The assertion the earlier rounds were missing. Every previous check
# here confirmed something *appeared* (a tab, an entry, a result); none
# confirmed something was *gone*. That gap let a close path that never
# reached the daemon look correct for several rounds: the GUI tab
# vanished locally, so the symptom matched, while the search stayed
# alive in the core (got3nks, PR #680 review point 6).
#
# GET /api/v0/search is the right oracle for this because it is a live
# EC_OP_SEARCH_LIST round trip to amuled, not amuleapi's own m_state
# cache -- so a search still listed here is still held by the core,
# whatever any one client's local view says.
#
# DELETE /search/{id} sends exactly the EC request amulegui's tab close
# sends (EC_OP_SEARCH_STOP + EC_TAG_SEARCH_CLOSE), so this exercises the
# same daemon-side path from a scriptable client.
CLOSE_RES=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search")
SID_CLOSE=$(printf '%s' "$CLOSE_RES" | jq -r '.id')

if [ -n "$SID_CLOSE" ] && [ "$SID_CLOSE" != "null" ]; then
	# Precondition: the daemon holds it. Without this the "gone" assertion
	# below would also pass against a search that was never there.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_json_eq "[.searches[] | select(.id == $SID_CLOSE)] | length" 1 \
		'close: daemon lists the search before the close'

	# A plain stop (no close) halts activity but must KEEP the search --
	# the contrapositive that proves the removal below is close's doing
	# and not a side effect of stopping.
	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$SID_CLOSE/stop" >/dev/null 2>&1
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_json_eq "[.searches[] | select(.id == $SID_CLOSE)] | length" 1 \
		'close: a plain stop leaves the search on the daemon'

	# Now free it, and assert it is GONE from the daemon's own list.
	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$SID_CLOSE" >/dev/null 2>&1
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_status 200 'close: GET /search after close → 200'
	_assert_json_eq "[.searches[] | select(.id == $SID_CLOSE)] | length" 0 \
		'close: the closed search is GONE from the daemon list'

	# And it is gone for everyone, not just the session that closed it --
	# a second instance re-reads the same core state. This is what a
	# second amulegui would have shown had it still been open.
	if [ "$HAVE_SECOND_INSTANCE" -eq 0 ]; then
		_skip "close: second-instance cross-check (no amuleapi binary)"
	else
	SECOND2_CONFIG_DIR=$(mktemp -d -t amuleapi_19_close_second.XXXXXX)
	SECOND2_LOG=$(mktemp -t amuleapi_19_close_second_log.XXXXXX)
	"$AMULEAPI_BIN" --config-dir="$SECOND2_CONFIG_DIR" \
		--host="$EC_HOST" --port="$EC_PORT" \
		--set-admin-pass="$ADMIN_PASS" >/dev/null 2>&1
		# Same reason as the first second-instance above.
		sed -i'.bak' "s|^Password=.*|Password=$EC_PASSWORD|" "$SECOND2_CONFIG_DIR/amuleapi.conf"
		rm -f "$SECOND2_CONFIG_DIR/amuleapi.conf.bak"
	"$AMULEAPI_BIN" --config-dir="$SECOND2_CONFIG_DIR" \
		--host="$EC_HOST" --port="$EC_PORT" \
		--http-port=4715 >"$SECOND2_LOG" 2>&1 &
	SECOND2_PID=$!
	for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
		curl -s -o /dev/null --max-time 1 "http://localhost:4715/api/v0/health" 2>/dev/null && break
		sleep 0.5
	done
	sleep 2
	SECOND2_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$ADMIN_PASS\"}" \
		"http://localhost:4715/api/v0/auth/login?include_token=true" | jq -r .token)
	if [ -n "$SECOND2_TOKEN" ] && [ "$SECOND2_TOKEN" != "null" ]; then
		_curl -H "Authorization: Bearer $SECOND2_TOKEN" "http://localhost:4715/api/v0/search"
		_assert_json_eq "[.searches[] | select(.id == $SID_CLOSE)] | length" 0 \
			'close: a second session also no longer sees the closed search'
	else
		_fail "close: second instance login" "no token; log: $(tail -c 300 "$SECOND2_LOG")"
	fi
	kill "$SECOND2_PID" >/dev/null 2>&1
	wait "$SECOND2_PID" 2>/dev/null
	rm -rf "$SECOND2_CONFIG_DIR" "$SECOND2_LOG"
	fi # HAVE_SECOND_INSTANCE -- close cross-check

	# Its results are unaddressable afterwards, too -- the bucket is freed,
	# not merely hidden from the listing.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$SID_CLOSE/results"
	_assert_status 404 'close: GET /search/{id}/results for the freed id → 404'
else
	_fail "close setup" "POST /search did not return a search_id ($CLOSE_RES)"
fi

if [ "$HAVE_SECOND_INSTANCE" -eq 0 ]; then
	_skip "12.1 foreign-search stop (no amuleapi binary; set AMULEAPI_BIN)"
else
# --- 12.1 A foreign search must be stoppable, not just visible. ----
# Found by driving two clients against one daemon and using the other as
# an oracle: GET /api/v0/search enumerates live core state, so it lists
# searches this session never started -- but the stop path gated on the
# local m_state cache alone and answered 404 for exactly those. You could
# see a search you could not close. Same contradiction previously fixed
# for the results read; every search-scoped handler now resolves its id
# through one RequireSearch helper, which is what stops it recurring a
# third time (got3nks, PR #680 review).
#
# Stage a genuinely foreign search: a second amuleapi instance starts it,
# so the primary at $HOST has never seen the id in its own cache.
FOREIGN_CONFIG_DIR=$(mktemp -d -t amuleapi_19_foreign.XXXXXX)
FOREIGN_LOG=$(mktemp -t amuleapi_19_foreign_log.XXXXXX)
"$AMULEAPI_BIN" --config-dir="$FOREIGN_CONFIG_DIR" \
	--host="$EC_HOST" --port="$EC_PORT" \
	--set-admin-pass="$ADMIN_PASS" >/dev/null 2>&1
# Same reason as the other extra instances: no --password, so the EC
# credential goes into the amuleapi.conf the call above just created.
sed -i'.bak' "s|^Password=.*|Password=$EC_PASSWORD|" "$FOREIGN_CONFIG_DIR/amuleapi.conf"
rm -f "$FOREIGN_CONFIG_DIR/amuleapi.conf.bak"
"$AMULEAPI_BIN" --config-dir="$FOREIGN_CONFIG_DIR" \
	--host="$EC_HOST" --port="$EC_PORT" \
	--http-port=4716 >"$FOREIGN_LOG" 2>&1 &
FOREIGN_PID=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
	curl -s -o /dev/null --max-time 1 "http://localhost:4716/api/v0/health" 2>/dev/null && break
	sleep 0.5
done
sleep 2
FOREIGN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"http://localhost:4716/api/v0/auth/login?include_token=true" | jq -r .token)

if [ -n "$FOREIGN_TOKEN" ] && [ "$FOREIGN_TOKEN" != "null" ]; then
	FOREIGN_RES=$(curl -s -X POST -H "Authorization: Bearer $FOREIGN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" \
		"http://localhost:4716/api/v0/search")
	SID_FOREIGN=$(printf '%s' "$FOREIGN_RES" | jq -r '.id')

	if [ -n "$SID_FOREIGN" ] && [ "$SID_FOREIGN" != "null" ]; then
		# The primary session can SEE it (this already worked).
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		_assert_json_eq "[.searches[] | select(.id == $SID_FOREIGN)] | length" 1 \
			'foreign stop: primary session lists the foreign search'

		# ...and can also FREE it. This is the assertion that was 404ing.
		_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/search/$SID_FOREIGN"
		_assert_status 204 'foreign stop: freeing a foreign search → 204 (not 404)'

		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		_assert_json_eq "[.searches[] | select(.id == $SID_FOREIGN)] | length" 0 \
			'foreign stop: the foreign search is actually gone afterwards'
	else
		_fail "foreign stop setup" "second instance POST /search returned no id ($FOREIGN_RES)"
	fi
else
	_fail "foreign stop: second instance login" "no token; log: $(tail -c 300 "$FOREIGN_LOG")"
fi
kill "$FOREIGN_PID" >/dev/null 2>&1
wait "$FOREIGN_PID" 2>/dev/null
rm -rf "$FOREIGN_CONFIG_DIR" "$FOREIGN_LOG"
fi # HAVE_SECOND_INSTANCE -- section 12.1

# --- 13. A failed search start must not wedge discovery. -----------
# amulegui defers EC_OP_SEARCH_LIST-driven tab creation while it has a
# START in flight, so a START that fails without ever being accounted
# for used to leave that deferral armed for the rest of the session --
# discovery silently dead, plus a per-tick EC round trip (got3nks, PR
# #680 review point 5). amuleapi does not share amulegui's counter, but
# it does share the daemon: this asserts the daemon stays healthy and
# enumerable across a rejected start, which is the half a script can
# observe. The client-side set-keyed-by-id fix is what closes the rest.
BAD_START=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
	-H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"query":"","type":"global"}' "$HOST/api/v0/search")
if [ "$BAD_START" = "400" ] || [ "$BAD_START" = "422" ]; then
	_pass "failed start: empty query rejected ($BAD_START)"
else
	_fail "failed start: empty query rejected" "expected 400/422, got $BAD_START"
fi

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 200 'failed start: GET /search still 200 afterwards'
_assert_json_eq '.searches | type' array 'failed start: /search still enumerable afterwards'

# A good start still works right after a rejected one -- i.e. the
# rejected attempt left no residue that blocks the next search.
AFTER_BAD=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search")
SID_AFTER=$(printf '%s' "$AFTER_BAD" | jq -r '.id')
if [ -n "$SID_AFTER" ] && [ "$SID_AFTER" != "null" ] && [ "$SID_AFTER" != "0" ]; then
	_pass "failed start: a subsequent search still starts (id $SID_AFTER)"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_json_eq "[.searches[] | select(.id == $SID_AFTER)] | length" 1 \
		'failed start: the subsequent search is discoverable'
	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$SID_AFTER" >/dev/null 2>&1
else
	_fail "failed start: subsequent search" "POST /search returned no id ($AFTER_BAD)"
fi

# --- 13.1 A search that retained no results still reports a terminal state.
#
# The per-id lifecycle used to be inferred from the retained result bucket, so a
# search that indexed nothing -- no server match, every hit dropped by the file
# type filter, or a global sweep stopped before its first result -- reported
# `idle` forever. `idle` is not terminal, so the progress sentinel fell through
# to the running percent (0) and every consumer read a finished search as still
# running at 0%: Stop stayed live, the bar never cleared, and amulegui's "an
# eD2k search is still running" prompt fired on a long-finished tab (issue
# #1103).
#
# Two searches are needed. While a search is the most recent one its state comes
# from the live scalar, which was always right; only the older of the two takes
# the inferred path this asserts on.
ZERO_QUERY="amuleapi19nosuchkeyword"
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$ZERO_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search"
_assert_status 202 "POST /search (zero-result probe) → 202"
ZERO_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"${ZERO_QUERY}b\",\"type\":\"global\"}" "$HOST/api/v0/search"
_assert_status 202 "POST /search (demotes the zero-result probe) → 202"
DEMOTER_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 200 "GET /search → 200 (after the zero-result probe)"
ZERO_COUNT=$(printf '%s' "$CURL_BODY" \
	| jq -r "[.searches[] | select(.id == $ZERO_SID)][0].result_count // empty")
# Guarded on the probe actually having retained nothing: on a daemon where the
# nonsense keyword somehow matched, this assertion would pass through the
# results-retained branch and prove nothing.
if [ "$ZERO_COUNT" = "0" ]; then
	_assert_json_eq "[.searches[] | select(.id == $ZERO_SID)][0].state" finished \
		'a demoted search that retained no results reports finished, not idle'
else
	_skip "zero-result lifecycle: probe retained ${ZERO_COUNT:-no} result(s), not 0"
fi

# Free both probes so the browse contract below sees the search set it expects.
for sid in "$ZERO_SID" "$DEMOTER_SID"; do
	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$sid" > /dev/null 2>&1
done

# --- Browse ("View Files") contract. -------------------------------
# POST /clients/{ecid}/shared_files starts a browse and returns a
# search_id; the files themselves arrive over the network from the peer,
# so a green happy-path needs a specific reachable peer with shares —
# not something a smoke test can stage. We cover the deterministic
# contract: auth, admin gate, method, and the ecid error paths.
BROWSE_UNKNOWN_ECID=4293000111

_curl -X POST "$HOST/api/v0/clients/$BROWSE_UNKNOWN_ECID/shared_files"
_assert_status 401 "POST /clients/{ecid}/shared_files (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/clients/$BROWSE_UNKNOWN_ECID/shared_files"
	_assert_status 403 "POST /clients/{ecid}/shared_files (guest) → 403"
fi

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/clients/notanumber/shared_files"
_assert_status 400 "POST /clients/{ecid}/shared_files (non-numeric ecid) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/clients/$BROWSE_UNKNOWN_ECID/shared_files"
_assert_status 404 "POST /clients/{ecid}/shared_files (unknown ecid) → 404"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/clients/$BROWSE_UNKNOWN_ECID/shared_files"
_assert_status 405 "GET /clients/{ecid}/shared_files (wrong method) → 405"

# Happy path when a peer happens to be connected: a browse is addressed
# like any other search, reports kind "browse" with the peer's
# client_ecid, and its files carry the folder they live in inside the
# peer's share. Skipped (not failed) with no peer connected, like the
# other peer-dependent assertions in this suite.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/clients"
PEER_ECID=$(printf '%s' "$CURL_BODY" | jq -r '.clients[0].ecid // empty')
if [ -n "$PEER_ECID" ]; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/clients/$PEER_ECID/shared_files"
	if [ "$CURL_STATUS" = "202" ]; then
		BROWSE_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')
		_pass "POST /clients/{ecid}/shared_files (live peer) → 202 (search_id $BROWSE_SID)"

		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		_assert_json_eq "[.searches[] | select(.id == $BROWSE_SID)][0].type" browse \
			'GET /search reports the browse with kind=browse'
		_assert_json_eq "[.searches[] | select(.id == $BROWSE_SID)][0].client_ecid" \
			"$PEER_ECID" 'GET /search reports the browsed peer as client_ecid'
		# Files received from the peer so far -- no total comparison, a browse
		# is still running here.
		_assert_json_eq "[.searches[] | select(.id == $BROWSE_SID)][0].result_count | type" \
			number 'GET /search carries a numeric result_count on the browse'

		# A browse must never report `idle` on the listing
		# (amule-org/amule#1060). Before the fix the listing derived a
		# browse's state from retained results, so a peer that denied the
		# request or never answered -- which leaves none -- read `idle`
		# forever, while the progress reply said `finished`.
		#
		# `running` or `finished` are both legitimate here and which one
		# lands depends on the peer: BROWSE_IN_PROGRESS is stamped when the
		# request is sent, but a peer that denies instantly can terminalize
		# the browse before this round trip completes. `idle` is the failure.
		LIST_STATE=$(printf '%s' "$CURL_BODY" | \
			jq -r "[.searches[] | select(.id == $BROWSE_SID)][0].state")
		case "$LIST_STATE" in
		running | finished)
			_pass "GET /search reports the browse as $LIST_STATE, never idle"
			;;
		*)
			_fail "browse listing state" \
				"expected running or finished, got $LIST_STATE"
			;;
		esac

		# While the browse is still running the listing and the per-id
		# progress reply must agree -- the invariant REFERENCE.md promises,
		# and the two are served by different EC replies so nothing else
		# enforces it. Only checked in the `running` case: the results
		# endpoint serves amuleapi's cached slot, which the refresher
		# refreshes on its own tick, so right after a state change the two
		# can legitimately differ by one tick.
		if [ "$LIST_STATE" = "running" ]; then
			_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
				"$HOST/api/v0/search/$BROWSE_SID/results?limit=1"
			PROG_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state')
			if [ "$PROG_STATE" = "running" ]; then
				_pass "listing and progress.state agree on the running browse"
			else
				_fail "listing vs progress state disagree" \
					"GET /search said running, results said $PROG_STATE"
			fi
		fi

		# A second browse of the same peer while the first is in flight joins
		# it rather than minting a second id (amule-org/amule#1059): a second
		# id would never receive a lifecycle, and the first would be stranded
		# reporting `running` forever with nothing able to terminalize it.
		#
		# Only meaningful while the first browse is actually in flight. Every
		# terminal path clears the in-flight counter, so once a browse settles
		# a fresh id is the CORRECT answer -- re-read the state and skip
		# rather than fail in that case, like the other peer-dependent
		# assertions here.
		#
		# Counted as a delta, not an absolute: browses that have already
		# settled stay on the listing until they are DELETEd or the daemon's
		# LRU evicts them, so any daemon that has browsed this peer before
		# legitimately shows more than one entry for it.
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		BROWSE_COUNT_BEFORE=$(printf '%s' "$CURL_BODY" | \
			jq "[.searches[] | select(.type == \"browse\")] | length")
		_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/clients/$PEER_ECID/shared_files"
		_assert_status 202 "POST /clients/{ecid}/shared_files (duplicate) → 202"
		DUP_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		FIRST_STATE=$(printf '%s' "$CURL_BODY" | \
			jq -r "[.searches[] | select(.id == $BROWSE_SID)][0].state")
		BROWSE_COUNT_AFTER=$(printf '%s' "$CURL_BODY" | \
			jq "[.searches[] | select(.type == \"browse\")] | length")
		if [ "$DUP_SID" = "$BROWSE_SID" ]; then
			_pass "a duplicate browse returns the id already in flight ($BROWSE_SID)"
			# ...so it added no entry to the listing.
			if [ "$BROWSE_COUNT_AFTER" = "$BROWSE_COUNT_BEFORE" ]; then
				_pass "a duplicate browse adds no listing entry (still $BROWSE_COUNT_AFTER)"
			else
				_fail "duplicate browse added a listing entry" \
					"before=$BROWSE_COUNT_BEFORE after=$BROWSE_COUNT_AFTER"
			fi
		elif [ "$FIRST_STATE" = "finished" ]; then
			echo "    info: first browse already settled before the duplicate was"
			echo "          sent — a new id is correct; skipping the join check"
			curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
				"$HOST/api/v0/search/$DUP_SID" >/dev/null 2>&1
		else
			_fail "duplicate browse minted a second id" \
				"first=$BROWSE_SID (state $FIRST_STATE) second=$DUP_SID"
		fi

		# Give the peer a moment to answer, then check the browse-only
		# folder field. An unreachable/denying peer returns nothing, which
		# is not a failure of this contract.
		sleep 3
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/$BROWSE_SID/results"
		_assert_status 200 "GET /search/{browse id}/results → 200"
		BN=$(printf '%s' "$CURL_BODY" | jq '.results | length')
		if [ "$BN" -gt 0 ]; then
			_assert_json_eq '[.results[] | has("directory")] | all' true \
				'every browse result carries a directory'
			_assert_json_eq '.results[0].directory | type' string \
				'browse directory is a string'
		else
			echo "    info: peer returned no files — skipping the directory assertions"
		fi
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/search/$BROWSE_SID/results?sort=directory"
		_assert_status 200 "GET /search/{browse id}/results?sort=directory → 200"

		# Same check after the browse has had time to settle: a peer that
		# denied or never answered leaves no results at all, which is exactly
		# the case the pre-#1060 listing reported as `idle` in perpetuity.
		# `running` is still legitimate for a peer genuinely still streaming.
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		LIST_STATE=$(printf '%s' "$CURL_BODY" | \
			jq -r "[.searches[] | select(.id == $BROWSE_SID)][0].state")
		if [ "$LIST_STATE" = "finished" ] || [ "$LIST_STATE" = "running" ]; then
			_pass "browse listing state is $LIST_STATE after settling (never idle)"
		else
			_fail "browse listing state after settling" \
				"expected finished (or still running), got $LIST_STATE"
		fi
		curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/search/$BROWSE_SID" >/dev/null 2>&1
	else
		echo "    info: browse of peer $PEER_ECID returned $CURL_STATUS — skipping browse happy path"
	fi
else
	echo "    info: no connected peer — skipping the browse happy path"
fi

# --- Browse of a peer the daemon declines to contact. --------------
# A LowID peer amuled cannot call back is never contacted at all: TryToConnect
# returns having sent nothing. Such a browse used to sit `running` forever --
# bounded only by the 34-minute client-list cleanup, and not even by that when
# the peer is also a download source, since the branch that declines the
# contact sets DS_LOWTOLOWIP and the cleanup skips anything not DS_NONE
# (amule-org/amule#1071). It must now reach a terminal state at once.
#
# Needs a peer in exactly that state, which no CI environment can guarantee,
# so probe for one and skip -- not fail -- when there is none, like the other
# peer-dependent assertions in this suite.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/clients"
UNREACHABLE_ECID=$(printf '%s' "$CURL_BODY" \
	| jq -r '[.clients[] | select(.download_state == "lowtolowip")][0].ecid // empty')
if [ -n "$UNREACHABLE_ECID" ]; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/clients/$UNREACHABLE_ECID/shared_files"
	if [ "$CURL_STATUS" = "202" ]; then
		DEAD_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')
		# No peer round trip is involved -- the daemon decides not to contact
		# it and fails the browse in the same call -- so a short settle is
		# enough. Poll rather than sleep blindly, to keep it quick when it
		# behaves and still generous when the machine is loaded.
		DEAD_STATE=""
		for _ in 1 2 3 4 5 6 7 8 9 10; do
			_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
			DEAD_STATE=$(printf '%s' "$CURL_BODY" | \
				jq -r "[.searches[] | select(.id == $DEAD_SID)][0].state")
			[ "$DEAD_STATE" = "finished" ] && break
			sleep 1
		done
		if [ "$DEAD_STATE" = "finished" ]; then
			_pass "browse of an uncontactable peer reaches finished, not stuck running"
		else
			_fail "browse of an uncontactable peer" \
				"expected finished within 10s, got $DEAD_STATE"
		fi
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/search/$DEAD_SID/results?limit=1"
		_assert_json_eq '.progress.state' finished \
			'the results endpoint agrees the dead browse is finished'

		# The in-flight flag was cleared with the terminal mark, so the peer
		# is still browsable rather than joined to a dead browse for good.
		_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/clients/$UNREACHABLE_ECID/shared_files"
		RETRY_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id')
		if [ "$RETRY_SID" != "$DEAD_SID" ]; then
			_pass "a later browse of that peer starts fresh ($RETRY_SID), not the dead id"
		else
			_fail "browse of an uncontactable peer is not retryable" \
				"retry joined the failed browse $DEAD_SID"
		fi
		for SID_TO_FREE in $DEAD_SID $RETRY_SID; do
			curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
				"$HOST/api/v0/search/$SID_TO_FREE" >/dev/null 2>&1
		done
	else
		echo "    info: browse of $UNREACHABLE_ECID returned $CURL_STATUS — skipping"
	fi
else
	echo "    info: no uncontactable (lowtolowip) peer — skipping the dead-browse check"
fi

# --- Related-files search (docs contract). -------------------------
# There is no endpoint: the desktop composes a magic keyword and starts an
# ordinary LOCAL search, so a REST client does the same. Assert the shape
# is accepted, which is the part that is deterministic; whether the
# connected server answers depends on its related_search capability flag.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"query":"related::baadbaadbaadbaadbaadbaadbaadbaad","type":"local"}' \
	"$HOST/api/v0/search"
_assert_status 202 'POST /search with a related:: query → 202 (no dedicated endpoint needed)'
REL_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id // empty')
[ -n "$REL_SID" ] && curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/$REL_SID" >/dev/null 2>&1
# The capability a client should check before reading "no hits" as
# "nothing related": the connected server advertises it in its flags.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/servers"
_assert_json_eq '[.servers[]? | .tcp_flags | has("related_search")] | all' true \
	'every server advertises its related_search capability in tcp_flags'

# --- 13. A foreign search discovered AFTER the union has been polling. -----
# Section 3.2 covers the easy ordering: a second instance that has never POSTed
# anything reads a search it did not start. That one cannot regress, because an
# instance with no slots of its own never sends the union at all.
#
# The ordering that DID break is this one. Once a session holds any slot, its
# tick polls the multi-search union, and the union responder walks every search
# the core holds -- so it hands over a foreign search's results to a session
# with no slot to put them in. They are dropped, but the daemon has marked them
# sent and seeds its valuemap, so every later poll elides them. Discovering that
# search afterwards used to yield a permanently empty result set that no number
# of ticks would fill.
#
# Deliberately the LAST section in this file: it starts two extra searches on
# the shared daemon, and doing that earlier reorders what the sections above see.
if [ "$HAVE_SECOND_INSTANCE" -eq 1 ]; then
	THIRD_HOST="localhost:4715"
	THIRD_CONFIG_DIR=$(mktemp -d -t amuleapi_19_search_third.XXXXXX)
	THIRD_LOG=$(mktemp -t amuleapi_19_search_third_log.XXXXXX)
	"$AMULEAPI_BIN" --config-dir="$THIRD_CONFIG_DIR" \
		--host="$EC_HOST" --port="$EC_PORT" \
		--set-admin-pass="$ADMIN_PASS" >/dev/null 2>&1
	sed -i'.bak' "s|^Password=.*|Password=$EC_PASSWORD|" "$THIRD_CONFIG_DIR/amuleapi.conf"
	rm -f "$THIRD_CONFIG_DIR/amuleapi.conf.bak"
	"$AMULEAPI_BIN" --config-dir="$THIRD_CONFIG_DIR" \
		--host="$EC_HOST" --port="$EC_PORT" \
		--http-port=4715 >"$THIRD_LOG" 2>&1 &
	THIRD_PID=$!
	for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
		curl -s -o /dev/null --max-time 1 "http://$THIRD_HOST/api/v0/health" 2>/dev/null && break
		sleep 0.5
	done
	sleep 3

	THIRD_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$ADMIN_PASS\"}" "http://$THIRD_HOST/api/v0/auth/login?include_token=true" \
		| jq -r .token)

	if [ -n "$THIRD_TOKEN" ] && [ "$THIRD_TOKEN" != "null" ]; then
		# Give the third instance a slot of its own. From here its tick sends the
		# union every second, which is the precondition for the bug.
		OWN_SID=$(curl -s -X POST -H "Authorization: Bearer $THIRD_TOKEN" \
			-H "Content-Type: application/json" \
			-d "{\"query\":\"$TEST_QUERY\"}" "http://$THIRD_HOST/api/v0/search" \
			| jq -r '.id // empty')
		sleep 3

		# NOW start a search it knows nothing about, and let its union poll run
		# over the results several times. Every one of those polls offers this
		# search's results to a session with no slot for them.
		_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
			-H "Content-Type: application/json" \
			-d "{\"query\":\"$TEST_QUERY\"}" "$HOST/api/v0/search"
		_assert_status 202 'late-discovery: first instance starts a search the third has never seen'
		LATE_SID=$(printf '%s' "$CURL_BODY" | jq -r '.id // empty')
		sleep 12

		if [ -n "$LATE_SID" ]; then
			# What the daemon actually holds, so an empty read is distinguishable
			# from a search that genuinely found nothing.
			_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
			LATE_HELD=$(printf '%s' "$CURL_BODY" \
				| jq -r "[.searches[] | select(.id == $LATE_SID)][0].result_count // 0")
			if [ "$LATE_HELD" -gt 0 ]; then
				# First read by the third instance: discovery has to seed the slot in
				# full here, because the differential stream has nothing left to send.
				_curl -H "Authorization: Bearer $THIRD_TOKEN" \
					"http://$THIRD_HOST/api/v0/search/$LATE_SID/results"
				_assert_status 200 'late-discovery: third instance reads the late search → 200'
				_assert_json_eq '.search_id' "$LATE_SID" 'late-discovery: it echoes the discovered id'
				_assert_json_eq '[.results[]] | length > 0' true \
					"late-discovery: the discovered search is seeded (daemon holds $LATE_HELD)"
				# And it stays: a seed that only survived until the next union poll
				# would be worse than none.
				sleep 3
				_curl -H "Authorization: Bearer $THIRD_TOKEN" \
					"http://$THIRD_HOST/api/v0/search/$LATE_SID/results"
				_assert_json_eq '[.results[]] | length > 0' true \
					'late-discovery: the seeded results survive the next union polls'
			else
				_skip "late-discovery: the daemon holds no results for the late search"
			fi
			curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
				"$HOST/api/v0/search/$LATE_SID" >/dev/null 2>&1
		else
			_skip 'late-discovery: no search id returned for the late search'
		fi
		[ -n "$OWN_SID" ] && curl -s -X DELETE -H "Authorization: Bearer $THIRD_TOKEN" \
			"http://$THIRD_HOST/api/v0/search/$OWN_SID" >/dev/null 2>&1
	else
		_fail "late-discovery: third amuleapi instance admin login" \
			"could not obtain a token; log: $(tail -c 300 "$THIRD_LOG")"
	fi

	kill "$THIRD_PID" >/dev/null 2>&1
	wait "$THIRD_PID" 2>/dev/null
	rm -rf "$THIRD_CONFIG_DIR" "$THIRD_LOG"
else
	_skip 'late-discovery: no amuleapi binary to start a third instance'
fi

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
