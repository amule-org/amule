#!/usr/bin/env bash
#
# amuleapi 07-read-stats-and-search-results — /stats/tree, /stats/graphs/{graph}, /search/results.
# /stats/tree is a recursive structure; /stats/graphs is a time-series with
# per-graph path-param + ?width=N tailing; /search/results is read-only
# until Phase 5 adds POST /search.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_07_read_stats_and_search_results_body.XXXXXX)
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

echo "amuleapi 07-read-stats-and-search-results smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Wait for the refresher to populate the cache (3 new EC roundtrips
# plus the existing tick, so the first full snapshot lands by tick 2).
sleep 4

# --- 1. Auth gate. -------------------------------------------------
for ep in stats/tree stats/graphs/download search/results; do
	_curl "$HOST/api/v0/$ep"
	_assert_status 401 "GET /$ep (no creds) → 401"
done

# --- 2. /stats/tree shape. -----------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/tree"
_assert_status 200 "GET /stats/tree → 200"
_assert_json_eq '.nodes | type'            array  '/stats/tree .nodes is array'
# amuled's stats tree always has at least Uptime + Transfer + Connection
# at the top level — assert a non-empty `nodes` and a labeled first
# entry rather than pinning specific text (locale-dependent).
_assert_json_eq '.nodes | length > 0'      true   '/stats/tree has at least one top-level node'
_assert_json_eq '.nodes[0].label | type'   string '/stats/tree first node has a label'
_assert_json_eq '.nodes[0].values | type'  array  '/stats/tree first node has a values array'
_assert_json_eq '.nodes[0].children | type' array '/stats/tree first node has a children array'
# Typed values: labels are English "%s" templates, values carry the raw typed
# data (integer/bytes/time/…), so the payload is locale-independent. Assert at
# least one typed value exists and every value type is from the known set.
_assert_json_eq '[.. | .values? // empty | .[]?] | length > 0' true \
	'/stats/tree carries at least one typed value'
_assert_json_eq '([.. | .values? // empty | .[]? | .type] | unique)
	- ["integer","istring","bytes","ishort","time","speed","string","double"]
	| length == 0' true '/stats/tree value types are all from the known set'
# Stable machine keys: locale-independent identifiers on the fixed skeleton
# nodes, safe to pin (unlike labels). Uptime is always the first node.
_assert_json_eq '.nodes[0].key' uptime '/stats/tree first node key is "uptime"'
_assert_json_eq '[.. | objects | select(has("key")) | .key] as $k
	| (["ul_dl_ratio","download_data","servers_working"] | all(($k | index(.)) != null))' \
	true '/stats/tree carries the expected stable keys'
_assert_json_eq '[.. | objects | select(has("key")) | .key]
	| (length > 0) and (length == (unique | length))' \
	true '/stats/tree keys are present and unique'
# The ratio node keeps its composite string value for legacy consumers...
_assert_json_eq '[.. | objects | select(.key? == "ul_dl_ratio") | .values[0].type] | .[0]' \
	string '/stats/tree ratio node still carries its composite string value'
# ...and when the daemon can compute it, exposes numeric ratio fields. Absent
# on a freshly-started daemon with no transfer, so assert shape, not presence.
_assert_json_eq '[.. | objects | select(has("ratio")) | .ratio | (.session, .total)
	| select(. != null) | type] | all(. == "number")' \
	true '/stats/tree ratio fields, when present, are numbers'

# --- 3. /stats/graphs/{graph} — all four named graphs. -------------
for g in download upload connections kad; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/$g"
	_assert_status 200 "GET /stats/graphs/$g → 200"
	_assert_json_eq '.graph'                    "$g"  "/stats/graphs/$g reports graph=$g"
	_assert_json_eq '.interval_seconds | type'  number "/stats/graphs/$g interval_seconds is numeric"
	_assert_json_eq '.points | type'            array  "/stats/graphs/$g .points is array"
	_assert_json_eq '.session | type'           object "/stats/graphs/$g .session is object"
done
# Per-graph unit mapping.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download"
_assert_json_eq '.unit' bps   '/stats/graphs/download reports unit=bps'
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/connections"
_assert_json_eq '.unit' count '/stats/graphs/connections reports unit=count'

# --- 4. /stats/graphs/{graph} ?width=N tailing. --------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download?width=5"
_assert_status 200 "GET /stats/graphs/download?width=5 → 200"
_assert_json_eq '.points | length <= 5' true \
	'/stats/graphs/download?width=5 returns ≤5 points'
# When a point exists, it must carry both t (ISO-8601) and t_unix.
if [ "$(printf '%s' "$CURL_BODY" | jq '.points | length')" -gt 0 ]; then
	_assert_json_eq '.points[0].t | length' 20 \
		'/stats/graphs/download point.t is 20-char ISO-8601'
	_assert_json_eq '.points[0].t_unix | type' number \
		'/stats/graphs/download point.t_unix is numeric'
	_assert_json_eq '.points[0].value | type' number \
		'/stats/graphs/download point.value is numeric'
fi

# --- 5. Unknown graph name → 404. ----------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/bogus"
_assert_status 404 "GET /stats/graphs/bogus → 404"
_assert_json_eq '.error.code' not_found \
	'/stats/graphs/{unknown} carries error.code=not_found'

# --- 6. /search/results — empty until Phase 5's POST /search. ------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/search/results"
_assert_status 200 "GET /search/results → 200"
_assert_json_eq '.results | type'       array '/search/results .results is array'
# Tolerant of empty results; if a phase 5 search was triggered earlier
# and left state, the per-item shape still checks.
COUNT=$(printf '%s' "$CURL_BODY" | jq '.results | length')
if [ "$COUNT" -gt 0 ]; then
	_assert_json_eq '.results[0].hash | length' 32 \
		'/search/results[0].hash is 32-char hex'
	_assert_json_eq '.results[0].name | type'   string \
		'/search/results[0].name is string'
	_assert_json_eq '.results[0].sources | type' object \
		'/search/results[0].sources is object'
fi

# --- 7. Method gate. -----------------------------------------------
for ep in stats/tree stats/graphs/download search/results; do
	_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/$ep"
	_assert_status 405 "DELETE /api/v0/$ep → 405"
done

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
