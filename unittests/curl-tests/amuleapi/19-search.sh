#!/usr/bin/env bash
#
# amuleapi 19-search — search.
#
# Endpoints:
#   GET  /api/v0/search                                    — EC_OP_SEARCH_LIST
#   POST /api/v0/search                                   — EC_OP_SEARCH_START
#       body: {query, type?, file_type?, extension?,
#              min_size?, max_size?, min_avail?}
#   POST /api/v0/search/stop                              — EC_OP_SEARCH_STOP
#   GET  /api/v0/search/results                            — read accumulated
#   POST /api/v0/search/results/{hash}/download           — EC_OP_DOWNLOAD_SEARCH_RESULT
#       body: {category?: uint8} (optional)
#   GET  /api/v0/search/results/{hash}/comments           — Kad ratings/comments for a result
#   POST /api/v0/search/results/{hash}/comments           — EC_OP_SHARED_FILE_SEARCH_KAD_NOTES
#   POST /api/v0/clients/{ecid}/shared_files              — browse a peer ("View Files"), returns a search_id
#
# /search/results is no longer a per-GET fetch — POST /search marks
# the search active in state and the refresher polls amuled every
# tick while it stays active. GET /search/results reads straight
# from that state, so subsequent polls already see the fresh query's
# growing results without any cache coordination.
#
# amuled's SEARCH_START is async: results trickle in from servers /
# Kad over the next several seconds. Smoke polls /search/results with
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
AMULEAPI_BIN=${AMULEAPI_BIN:-$(cd "$(dirname "$0")/../../.." && pwd)/build-macos/src/webapi/amuleapi}

# A query likely to return results on any operator's daemon connected
# to ed2k. "ubuntu" is a safe choice — well-seeded across the network.
TEST_QUERY="ubuntu"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_19_search_body.XXXXXX)
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

echo "amuleapi 19-search smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
_curl "$HOST/api/v0/search"
_assert_status 401 "GET /search (no token) → 401"

_curl -X POST -H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\"}" "$HOST/api/v0/search"
_assert_status 401 "POST /search (no token) → 401"

_curl -X POST "$HOST/api/v0/search/stop"
_assert_status 401 "POST /search/stop (no token) → 401"

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
	-d "{\"query\":\"$TEST_QUERY\",\"min_size\":-1}" "$HOST/api/v0/search"
_assert_status 400 "POST /search (negative min_size) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json' "$HOST/api/v0/search"
_assert_status 400 "POST /search (malformed JSON) → 400"

# --- 3. POST /search happy + per-search_id addressing. ---------
#
# Multi-search: each POST /search gets its own daemon-allocated search_id
# and its own result slot; a new search does NOT wipe the others. The
# no-id GET /search/results resolves to the CURRENT (most-recently-started)
# search, so the polling loop below sees the new query's results fill up.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
_assert_status 200 "GET /search/results (baseline before POST) → 200"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search"
_assert_status 202 "POST /search (query=$TEST_QUERY, type=global) → 202"
_assert_json_eq '.ok'    true         'POST /search response.ok==true'
_assert_json_eq '.query' "$TEST_QUERY" 'POST /search echoes query'
_assert_json_eq '.search_id | type' number 'POST /search returns a numeric search_id'
FIRST_SID=$(printf '%s' "$CURL_BODY" | jq -r '.search_id')
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
_assert_json_eq '.search_id' "$FIRST_SID" 'GET /search/results (no id) echoes the current search_id'

# --- 3.1 GET /api/v0/search enumerates the search just started. ---
# Reachability fix (issue #641): GET /api/v0/search reads live daemon
# state via EC_OP_SEARCH_LIST rather than this session's own m_state
# cache, so the search just started via POST /search must appear here
# too -- proving the two endpoints agree on what amuled currently holds.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 200 "GET /search → 200"
_assert_json_eq '.searches | type' array 'GET /search .searches is an array'
_assert_json_eq "[.searches[] | select(.search_id == $FIRST_SID)] | length" 1 \
	'GET /search lists the search just started via POST /search'
_assert_json_eq "[.searches[] | select(.search_id == $FIRST_SID)][0].query" "$TEST_QUERY" \
	'GET /search entry echoes the query'
_assert_json_eq "[.searches[] | select(.search_id == $FIRST_SID)][0].kind" global \
	'GET /search entry reports kind==global'
_assert_json_eq "[[.searches[] | select(.search_id == $FIRST_SID)][0].state] | inside([\"running\",\"finished\"])" \
	true 'GET /search entry state is running or finished (never idle for an active search)'

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/search"
	_assert_status 200 "GET /search (guest) → 200 (GUEST-readable)"
fi

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
	curl -s -o /dev/null --max-time 1 "http://$SECOND_HOST/api/v0/version" 2>/dev/null && break
	sleep 0.5
done
sleep 4

SECOND_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "http://$SECOND_HOST/api/v0/auth/login?type=bearer" \
	| jq -r .token)

if [ -n "$SECOND_TOKEN" ] && [ "$SECOND_TOKEN" != "null" ]; then
	_curl -H "Authorization: Bearer $SECOND_TOKEN" "http://$SECOND_HOST/api/v0/search"
	_assert_status 200 "second amuleapi instance: GET /search → 200"
	_assert_json_eq "[.searches[] | select(.search_id == $FIRST_SID)] | length" 1 \
		'second amuleapi instance (never POSTed) still lists the first instance'"'"'s search'

	_curl -H "Authorization: Bearer $SECOND_TOKEN" \
		"http://$SECOND_HOST/api/v0/search/results?search_id=$FIRST_SID"
	_assert_status 200 'second amuleapi instance: GET /search/results?search_id=<foreign id> → 200 (not 404)'
	_assert_json_eq '.search_id' "$FIRST_SID" 'second amuleapi instance /search/results echoes the discovered search_id'
else
	_fail "second amuleapi instance: admin login" "could not obtain a token; log: $(tail -c 300 "$SECOND_LOG")"
fi

kill "$SECOND_PID" >/dev/null 2>&1
wait "$SECOND_PID" 2>/dev/null
rm -rf "$SECOND_CONFIG_DIR" "$SECOND_LOG"

# --- 3.5 Regression: progress shouldn't claim finished right after POST. -
# amuled briefly reports raw=100 in the "queue-empty-at-start" window
# before the global-search timer populates m_serverQueue; if amuleapi
# trusted that raw value naively, GET /search/results right after POST
# would (incorrectly) say {progress:{percent:100, state:"finished"}}
# with results=[]. The refresher's state machine masks that window —
# this asserts the mask is in force.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
_assert_status 200 "GET /search/results immediately after POST → 200"
_assert_json_eq '.progress.state' running 'progress.state is "running" after POST /search'
_assert_json_eq '.progress.kind | type' string 'progress.kind is a string'

# --- 4. Poll /search/results until we get hits (max ~10 s). -------
RESULT_HASH=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
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
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
	_assert_json_eq '.results[0].hash | length' 32     '/search/results[0].hash is 32-char hex'
	_assert_json_eq '.results[0].name | type'   string '/search/results[0].name is string'
	_assert_json_eq '.results[0].size | type'   number '/search/results[0].size is numeric'
	# Download status + file type (issue #429).
	_assert_json_eq '[.results[0].status] | inside(["new","downloaded","queued","canceled","queued_canceled"])' \
		true '/search/results[0].status is a known enum value'
	_assert_json_eq '.results[0].type | type'   string '/search/results[0].type is string'
	# Media metadata (issue #430): an object when the hit is locally
	# known/probed, absent otherwise — both are valid.
	_assert_json_eq '.results[0].media | type | test("^(object|null)$")' \
		true '/search/results[0].media is an object or absent'
	# Result grouping (issue #431): every result carries a children[]
	# array (empty when the hit was seen under a single name). No result
	# is itself a child (children are folded into their parent), and each
	# child object has ecid + name + hash + sources.
	_assert_json_eq '.results[0].children | type' array '/search/results[0].children is an array'
	_assert_json_eq '[.results[].children[]?] | all(has("ecid") and has("name") and has("hash") and has("sources"))' \
		true 'every child has ecid/name/hash/sources'
	_assert_json_eq '[.results[].children[]?.hash | length] | all(. == 32)' \
		true 'every child hash is 32-char hex (shared with its parent)'

	# progress envelope. `progress` exists on every GET /search/results
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

# --- 5. POST /search/stop. ----------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/search/stop"
_assert_status 200 "POST /search/stop → 200"
_assert_json_eq '.ok' true 'search/stop response.ok==true'

# --- 6. POST /search/results/{hash}/download — happy + cleanup. --
if [ -n "$RESULT_HASH" ]; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"category":0}' \
		"$HOST/api/v0/search/results/$RESULT_HASH/download"
	_assert_status 202 "POST /search/results/{hash}/download → 202"
	_assert_json_eq '.ok'       true         'download response.ok==true'
	_assert_json_eq '.hash'     "$RESULT_HASH" 'download response echoes hash'
	_assert_json_eq '.category' 0            'download response category=0'

	# Empty-body POST should also succeed (category defaults to 0).
	# But first DELETE the just-created download so we don't trip
	# "already in queue".
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads/$RESULT_HASH"
	# 200 if found, 404 if already evicted by amuled — either is OK.
	if [ "$CURL_STATUS" = "200" ] || [ "$CURL_STATUS" = "404" ]; then
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
	-d '{"category":300}' \
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

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/stop"
_assert_status 405 "PATCH /search/stop → 405"

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

KAD_STATES=""; KAD_PCTS=""; SAW_RUNNING_KAD=0
for _ in 1 2 3 4 5 6; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
	ST=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state')
	KD=$(printf '%s' "$CURL_BODY" | jq -r '.progress.kind')
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

curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/stop" > /dev/null 2>&1

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
CMT_HASH=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
	N=$(printf '%s' "$CURL_BODY" | jq '.results | length')
	if [ "$N" -gt 0 ]; then
		CMT_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.results[0].hash')
		break
	fi
	sleep 0.25
done

if [ -n "$CMT_HASH" ]; then
	# Every result carries the comment fields on the list itself.
	_assert_json_eq '.results[0].kad_comment_search_running | type' boolean \
		'/search/results[0].kad_comment_search_running is boolean (issue #434)'
	_assert_json_eq '.results[0].comments | type' array \
		'/search/results[0].comments is an array'

	# Trigger an on-demand Kad notes lookup for the result.
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/results/$CMT_HASH/comments"
	_assert_status 202 "POST /search/results/{hash}/comments → 202"
	_assert_json_eq '.status' kad_search_started 'search comments POST status==kad_search_started'

	# Per-result comments view.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/results/$CMT_HASH/comments"
	_assert_status 200 "GET /search/results/{hash}/comments → 200"
	_assert_json_eq '.count | type' number 'search comments.count is numeric'
	_assert_json_eq '.kad_comment_search_running | type' boolean \
		'search comments carries kad_comment_search_running flag'
	_assert_json_eq '.comments | type' array 'search comments.comments is an array'

	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/stop" > /dev/null 2>&1
else
	echo "    info: 0 results — skipping search-comments happy path (daemon not connected)"
fi

# --- 11. Multi-search: concurrent searches, per-id addressing. ----
# amuleapi runs several searches at once, each with its own daemon-
# allocated search_id and its own result slot. Start an ed2k (global)
# AND a Kad search back-to-back and verify they coexist: distinct ids,
# per-id results, no-id => current, unknown id => 404, and stop+close
# frees one while the sibling survives.
#
# Regression guard (daemon fix): a Kad search started while an ed2k
# search is still in-flight must NOT stop/steal the ed2k search — its
# results are attributed via a scalar the Kad start used to clobber, so
# the global bucket would come back empty. Here we assert the global
# search still harvests while the Kad search runs alongside it.
G=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search")
SID_G=$(printf '%s' "$G" | jq -r '.search_id')
K=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"kad\"}" "$HOST/api/v0/search")
SID_K=$(printf '%s' "$K" | jq -r '.search_id')

if [ -n "$SID_G" ] && [ -n "$SID_K" ] && [ "$SID_G" != "null" ] && [ "$SID_K" != "null" ]; then
	if [ "$SID_G" != "$SID_K" ]; then
		_pass "Two concurrent searches get distinct search_ids ($SID_G, $SID_K)"
	else
		_fail "Concurrent search_ids" "both searches got the same id $SID_G"
	fi

	# Per-id progress kind reflects each search's own type.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results?search_id=$SID_G"
	_assert_status 200 "GET /search/results?search_id=<global> → 200"
	_assert_json_eq '.search_id'      "$SID_G" 'global search echoes its search_id'
	_assert_json_eq '.progress.kind'  global   'global search progress.kind==global'
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results?search_id=$SID_K"
	_assert_status 200 "GET /search/results?search_id=<kad> → 200"
	_assert_json_eq '.search_id'      "$SID_K" 'kad search echoes its search_id'
	_assert_json_eq '.progress.kind'  kad      'kad search progress.kind==kad'

	# no-id resolves to the current (most-recently-started == the Kad) search.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
	_assert_json_eq '.search_id' "$SID_K" 'no-id GET resolves to the current (latest) search'

	# Unknown / never-started id → 404 (distinct from known-but-empty).
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results?search_id=4293000111"
	_assert_status 404 "GET /search/results?search_id=<unknown> → 404"

	# Regression: the in-flight global search still harvests despite the
	# concurrent Kad search. Poll briefly; skip the assertion (don't fail)
	# if the daemon simply has no ed2k hits for the query.
	GN=0
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results?search_id=$SID_G"
		GN=$(printf '%s' "$CURL_BODY" | jq '.results | length')
		[ "$GN" -gt 0 ] && break
		sleep 0.25
	done
	if [ "$GN" -gt 0 ]; then
		_pass "In-flight global search still harvests alongside a Kad search ($GN hits)"
	else
		echo "    info: global search returned 0 alongside Kad — daemon may lack ed2k hits for '$TEST_QUERY'"
	fi

	# stop + close the global search: its slot is freed (404), the Kad
	# search is untouched (still 200).
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"search_id\":$SID_G,\"close\":true}" "$HOST/api/v0/search/stop"
	_assert_status 200 "POST /search/stop {search_id:<global>, close:true} → 200"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results?search_id=$SID_G"
	_assert_status 404 "GET closed global search → 404"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results?search_id=$SID_K"
	_assert_status 200 "sibling Kad search survives the close → 200"

	# stop with an explicit unknown id → 404.
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"search_id":4293000111}' "$HOST/api/v0/search/stop"
	_assert_status 404 "POST /search/stop {search_id:<unknown>} → 404"

	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"search_id\":$SID_K,\"close\":true}" "$HOST/api/v0/search/stop" >/dev/null 2>&1
else
	_fail "Multi-search setup" "POST /search did not return search_ids (G=$SID_G K=$SID_K)"
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
# POST /search/stop {close:true} sends exactly the EC request amulegui's
# tab close sends (EC_OP_SEARCH_STOP + EC_TAG_SEARCH_CLOSE), so this
# exercises the same daemon-side path from a scriptable client.
CLOSE_RES=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search")
SID_CLOSE=$(printf '%s' "$CLOSE_RES" | jq -r '.search_id')

if [ -n "$SID_CLOSE" ] && [ "$SID_CLOSE" != "null" ]; then
	# Precondition: the daemon holds it. Without this the "gone" assertion
	# below would also pass against a search that was never there.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_json_eq "[.searches[] | select(.search_id == $SID_CLOSE)] | length" 1 \
		'close: daemon lists the search before the close'

	# A plain stop (no close) halts activity but must KEEP the search --
	# the contrapositive that proves the removal below is close's doing
	# and not a side effect of stopping.
	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"search_id\":$SID_CLOSE}" "$HOST/api/v0/search/stop" >/dev/null 2>&1
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_json_eq "[.searches[] | select(.search_id == $SID_CLOSE)] | length" 1 \
		'close: a plain stop (no close) leaves the search on the daemon'

	# Now close it, and assert it is GONE from the daemon's own list.
	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"search_id\":$SID_CLOSE,\"close\":true}" "$HOST/api/v0/search/stop" >/dev/null 2>&1
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_status 200 'close: GET /search after close → 200'
	_assert_json_eq "[.searches[] | select(.search_id == $SID_CLOSE)] | length" 0 \
		'close: the closed search is GONE from the daemon list'

	# And it is gone for everyone, not just the session that closed it --
	# a second instance re-reads the same core state. This is what a
	# second amulegui would have shown had it still been open.
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
		curl -s -o /dev/null --max-time 1 "http://localhost:4715/api/v0/version" 2>/dev/null && break
		sleep 0.5
	done
	sleep 2
	SECOND2_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$ADMIN_PASS\"}" \
		"http://localhost:4715/api/v0/auth/login?type=bearer" | jq -r .token)
	if [ -n "$SECOND2_TOKEN" ] && [ "$SECOND2_TOKEN" != "null" ]; then
		_curl -H "Authorization: Bearer $SECOND2_TOKEN" "http://localhost:4715/api/v0/search"
		_assert_json_eq "[.searches[] | select(.search_id == $SID_CLOSE)] | length" 0 \
			'close: a second session also no longer sees the closed search'
	else
		_fail "close: second instance login" "no token; log: $(tail -c 300 "$SECOND2_LOG")"
	fi
	kill "$SECOND2_PID" >/dev/null 2>&1
	wait "$SECOND2_PID" 2>/dev/null
	rm -rf "$SECOND2_CONFIG_DIR" "$SECOND2_LOG"

	# Its results are unaddressable afterwards, too -- the bucket is freed,
	# not merely hidden from the listing.
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/results?search_id=$SID_CLOSE"
	_assert_status 404 'close: GET /search/results for the closed id → 404'
else
	_fail "close setup" "POST /search did not return a search_id ($CLOSE_RES)"
fi

# --- 12.1 A foreign search must be stoppable, not just visible. ----
# Found by driving two clients against one daemon and using the other as
# an oracle: GET /api/v0/search enumerates live core state, so it lists
# searches this session never started -- but POST /search/stop gated on
# the local m_state cache alone and answered 404 for exactly those. You
# could see a search you could not close. Same contradiction previously
# fixed for /search/results; the shared DiscoverSearchIfHeldByCore helper
# is what stops it recurring a third time (got3nks, PR #680 review).
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
	curl -s -o /dev/null --max-time 1 "http://localhost:4716/api/v0/version" 2>/dev/null && break
	sleep 0.5
done
sleep 2
FOREIGN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"http://localhost:4716/api/v0/auth/login?type=bearer" | jq -r .token)

if [ -n "$FOREIGN_TOKEN" ] && [ "$FOREIGN_TOKEN" != "null" ]; then
	FOREIGN_RES=$(curl -s -X POST -H "Authorization: Bearer $FOREIGN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" \
		"http://localhost:4716/api/v0/search")
	SID_FOREIGN=$(printf '%s' "$FOREIGN_RES" | jq -r '.search_id')

	if [ -n "$SID_FOREIGN" ] && [ "$SID_FOREIGN" != "null" ]; then
		# The primary session can SEE it (this already worked).
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		_assert_json_eq "[.searches[] | select(.search_id == $SID_FOREIGN)] | length" 1 \
			'foreign stop: primary session lists the foreign search'

		# ...and can also CLOSE it. This is the assertion that was 404ing.
		_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
			-H "Content-Type: application/json" \
			-d "{\"search_id\":$SID_FOREIGN,\"close\":true}" "$HOST/api/v0/search/stop"
		_assert_status 200 'foreign stop: closing a foreign search → 200 (not 404)'

		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
		_assert_json_eq "[.searches[] | select(.search_id == $SID_FOREIGN)] | length" 0 \
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
SID_AFTER=$(printf '%s' "$AFTER_BAD" | jq -r '.search_id')
if [ -n "$SID_AFTER" ] && [ "$SID_AFTER" != "null" ] && [ "$SID_AFTER" != "0" ]; then
	_pass "failed start: a subsequent search still starts (id $SID_AFTER)"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
	_assert_json_eq "[.searches[] | select(.search_id == $SID_AFTER)] | length" 1 \
		'failed start: the subsequent search is discoverable'
	curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"search_id\":$SID_AFTER,\"close\":true}" "$HOST/api/v0/search/stop" >/dev/null 2>&1
else
	_fail "failed start: subsequent search" "POST /search returned no id ($AFTER_BAD)"
fi

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

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
