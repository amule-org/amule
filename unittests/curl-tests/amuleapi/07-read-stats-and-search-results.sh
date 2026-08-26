#!/usr/bin/env bash
#
# amuleapi 07-read-stats-and-search-results — /stats/tree, /stats/graphs/{graph}, /search/{id}/results.
# /stats/tree is a recursive structure; /stats/graphs is a time-series with
# per-graph path-param + ?width=N tailing; /search/{id}/results is read-only
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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
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

# A search to address. Every search-scoped path names its id, so this
# script starts one rather than relying on a removed implicit default.
_curl -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
	-d '{"query":"amuleapi-phase07","type":"local"}' "$HOST/api/v0/search"
SID=$(printf '%s' "$CURL_BODY" | jq -r '.search_id // empty')
[ -n "$SID" ] || _die "POST /search returned no search_id"

# --- 1. Auth gate. -------------------------------------------------
for ep in stats/tree stats/graphs/download_speed "search/$SID/results"; do
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
	- ["integer","bytes","time","speed","string","double"]
	| length == 0' true '/stats/tree value types are all from the known set'
# Stable machine keys: locale-independent identifiers on the fixed skeleton
# nodes, safe to pin (unlike labels). Uptime is always the first node.
_assert_json_eq '.nodes[0].key' uptime '/stats/tree first node key is "uptime"'
_assert_json_eq '[.. | objects | select(has("key")) | .key] as $k
	| (["ul_dl_ratio","download_data","servers_working"] | all(($k | index(.)) != null))' \
	true '/stats/tree carries the expected stable keys'
# Skeleton keys are unique; the dynamic per-client-software rows deliberately
# share a key by kind (client_version / client_os), so exclude those.
_assert_json_eq '[.. | objects | select(has("key")) | .key]
	| map(select(. != "client_version" and . != "client_os"))
	| (length > 0) and (length == (unique | length))' \
	true '/stats/tree skeleton keys are present and unique'
# The ratio node keeps its composite string value for legacy consumers...
_assert_json_eq '[.. | objects | select(.key? == "ul_dl_ratio") | .values[0].type] | .[0]' \
	string '/stats/tree ratio node still carries its composite string value'
# ...and when the daemon can compute it, exposes numeric ratio fields. Absent
# on a freshly-started daemon with no transfer, so assert shape, not presence.
_assert_json_eq '[.. | objects | select(has("ratio")) | .ratio | (.session, .total)
	| select(. != null) | type] | all(. == "number")' \
	true '/stats/tree ratio fields, when present, are numbers'
# label_value: the untranslated version/OS value on per-client-software rows.
# `null` on every other node rather than absent, so the presence test is
# `!= null` -- the key is always there. Assert shape, not presence: it is only
# non-null once peers with version/OS info have connected.
_assert_json_eq '[.. | objects | select(has("label_value")) | .label_value] | all(. == null or type == "string")' \
	true '/stats/tree label_value is a string or null on every node'
# Every node carries the key, so a client can read it without a has() guard.
_assert_json_eq '[.. | objects | select(has("label") and has("values")) | has("label_value")] | all(.)' \
	true '/stats/tree label_value is present on every node, null where there is no datum'
# token: additive locale-independent marker on well-known sentinel string
# values ("never"/"not_available"), and `null` on every other value. Assert the
# non-null tokens are from the known set and always accompany a string value
# (the English value is kept alongside).
_assert_json_eq '([.. | objects | .values? // empty | .[]? | .token | select(. != null)]
	| unique) - ["never","not_available"] | length == 0' \
	true '/stats/tree value tokens are from the known set'
_assert_json_eq '[.. | objects | .values? // empty | .[]? | select(.token != null)
	| (.type == "string" and (.value | type) == "string")] | all(.)' \
	true '/stats/tree value tokens ride on a string value (kept for legacy clients)'
# Same for the two optional value keys: always present, null when there is
# nothing to report.
_assert_json_eq '[.. | objects | .values? // empty | .[]? | (has("token") and has("extra"))] | all(.)' \
	true '/stats/tree values always carry token and extra, null where absent'

# --- 3. /stats/graphs/{graph} — all four named graphs. -------------
for g in download_speed upload_speed connections kad_nodes; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/$g"
	_assert_status 200 "GET /stats/graphs/$g → 200"
	_assert_json_eq '.graph'                    "$g"  "/stats/graphs/$g reports graph=$g"
	_assert_json_eq '.interval_seconds | type'  number "/stats/graphs/$g interval_seconds is numeric"
	_assert_json_eq '.points | type'            array  "/stats/graphs/$g .points is array"
	_assert_json_eq '.session | type'           object "/stats/graphs/$g .session is object"
	_assert_json_eq '.max_points | type'        number "/stats/graphs/$g max_points is numeric"
	_assert_json_eq '(.points | length) <= .max_points' true \
		"/stats/graphs/$g never returns more points than max_points"
done
# Per-graph unit mapping.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download_speed"
_assert_json_eq '.unit' bytes_per_second '/stats/graphs/download_speed reports unit=bytes_per_second'
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/connections"
_assert_json_eq '.unit' count '/stats/graphs/connections reports unit=count'

# Session object: the two byte counters are scaled back from the KiB the
# daemon sends, kad is node-seconds rather than bytes, and duration is what
# turns any of them into an average.
_assert_json_eq '.session | has("download_bytes") and has("upload_bytes")
	and has("kad_node_seconds") and has("duration_seconds")' true \
	'/stats/graphs session carries the four corrected fields'
_assert_json_eq '.session | has("kad_bytes")' false \
	'/stats/graphs session no longer reports the misnamed kad_bytes'

# The connections graph carries the second data blob's two series when the
# daemon reports it; the other graphs never do.
if [ "$(printf '%s' "$CURL_BODY" | jq '.points | length')" -gt 0 ]; then
	_assert_json_eq '[.points[] | has("active_uploads")] | (all(.) or (any(.) | not))' true \
		'/stats/graphs/connections active_uploads is present on all points or none'
fi
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download_speed"
_assert_json_eq '[.points[]? | has("active_uploads")] | any(.) | not' true \
	'/stats/graphs/download_speed never carries the connections-only series'

# --- 3b. ?interval=N and its validation. ---------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download_speed?interval=10"
_assert_status 200 "GET /stats/graphs/download_speed?interval=10 → 200"
_assert_json_eq '.interval_seconds' 10 \
	'/stats/graphs?interval=10 reports the interval it applied'
for bad in 0 3601 abc; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download_speed?interval=$bad"
	_assert_status 400 "GET /stats/graphs/download_speed?interval=$bad → 400"
	_assert_json_eq '.error.code' bad_request \
		"/stats/graphs?interval=$bad carries error.code=bad_request"
done

# --- 3c. /stats/tree ?max_client_versions=N and its validation. ----
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/tree?max_client_versions=3"
_assert_status 200 "GET /stats/tree?max_client_versions=3 → 200"
for bad in -1 256 abc; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/tree?max_client_versions=$bad"
	_assert_status 400 "GET /stats/tree?max_client_versions=$bad → 400"
	_assert_json_eq '.error.code' bad_request \
		"/stats/tree?max_client_versions=$bad carries error.code=bad_request"
done

# --- 4. /stats/graphs/{graph} ?width=N tailing. --------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download_speed?width=5"
_assert_status 200 "GET /stats/graphs/download_speed?width=5 → 200"
_assert_json_eq '.points | length <= 5' true \
	'/stats/graphs/download_speed?width=5 returns ≤5 points'
# When a point exists, it must carry both t (ISO-8601) and t_unix.
if [ "$(printf '%s' "$CURL_BODY" | jq '.points | length')" -gt 0 ]; then
	_assert_json_eq '.points[0].t | length' 20 \
		'/stats/graphs/download_speed point.t is 20-char ISO-8601'
	_assert_json_eq '.points[0].t_unix | type' number \
		'/stats/graphs/download_speed point.t_unix is numeric'
	_assert_json_eq '.points[0].value | type' number \
		'/stats/graphs/download_speed point.value is numeric'
fi

# --- 5. Unknown graph name → 404. ----------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/bogus"
_assert_status 404 "GET /stats/graphs/bogus → 404"
_assert_json_eq '.error.code' not_found \
	'/stats/graphs/{unknown} carries error.code=not_found'

# --- 6. /search/{id}/results. --------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/search/$SID/results"
_assert_status 200 "GET /search/{id}/results → 200"
_assert_json_eq '.results | type'  array  '/search/{id}/results .results is array'
_assert_json_eq '.search_id'       "$SID" '/search/{id}/results echoes its search_id'
_assert_json_eq '.query'  amuleapi-phase07 '/search/{id}/results reports its query'
# Tolerant of empty results; a local search on a fresh daemon may find
# nothing. The per-item shape still checks when there is an item.
COUNT=$(printf '%s' "$CURL_BODY" | jq '.results | length')
if [ "$COUNT" -gt 0 ]; then
	_assert_json_eq '.results[0].hash | length' 32 \
		'/search/{id}/results[0].hash is 32-char hex'
	_assert_json_eq '.results[0].name | type'   string \
		'/search/{id}/results[0].name is string'
	_assert_json_eq '.results[0].sources | type' object \
		'/search/{id}/results[0].sources is object'
	_assert_json_eq '.results[0].directory | type' string \
		'/search/{id}/results[0].directory is string'
fi

# --- 7. Method gate. -----------------------------------------------
for ep in stats/tree stats/graphs/download_speed "search/$SID/results"; do
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
