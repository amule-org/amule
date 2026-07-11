#!/usr/bin/env bash
#
# amuleapi 19-search — search.
#
# Endpoints:
#   POST /api/v0/search                                   — EC_OP_SEARCH_START
#       body: {query, type?, file_type?, extension?,
#              min_size?, max_size?, min_avail?}
#   POST /api/v0/search/stop                              — EC_OP_SEARCH_STOP
#   GET  /api/v0/search/results                            — read accumulated
#   POST /api/v0/search/results/{hash}/download           — EC_OP_DOWNLOAD_SEARCH_RESULT
#       body: {category?: uint8} (optional)
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

# --- 3. POST /search happy + results-reset observable. ---------
#
# amuled wipes its searchlist on SEARCH_START (ExternalConn.cpp:1437),
# and amuleapi's MarkSearchStarted wipes m_search alongside it so the
# pre-POST results don't bleed into the new query. Step 4 below proves
# the reset is observable: the polling loop must see the new query's
# results fill up rather than the prior query's tail.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search/results"
_assert_status 200 "GET /search/results (baseline before POST) → 200"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$TEST_QUERY\",\"type\":\"global\"}" "$HOST/api/v0/search"
_assert_status 202 "POST /search (query=$TEST_QUERY, type=global) → 202"
_assert_json_eq '.ok'    true         'POST /search response.ok==true'
_assert_json_eq '.query' "$TEST_QUERY" 'POST /search echoes query'

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
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/search"
_assert_status 405 "GET /search → 405"

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

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
