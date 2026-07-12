#!/usr/bin/env bash
#
# amuleapi 10-refresher-lazy-ondemand — refresher slim-down: GET_ULOAD_QUEUE folded
# into GET_UPDATE, four lazy ops moved to on-demand TTL-cached fetch.
#
# Per-tick EC ops dropped from 8 to 3:
#   1. EC_OP_STAT_REQ @ EC_DETAIL_FULL     → /status + /kad + /logs/amule
#   2. EC_OP_GET_UPDATE @ EC_DETAIL_INC_UPDATE → /downloads + /shared +
#                                              /servers + /clients
#   3. EC_OP_GET_PREFERENCES                → /preferences + /categories
#
# Four endpoints lazy-fetched on first GET, coalesced via 1 s TTL:
#   * /logs/serverinfo  (EC_OP_GET_SERVERINFO)
#   * /stats/tree       (EC_OP_GET_STATSTREE)
#   * /stats/graphs/{X} (EC_OP_GET_STATSGRAPHS — one fetch serves all 4)
#   * /search/results   (EC_OP_SEARCH_RESULTS)
#
# Wire changes:
#   * /uploads RETIRED → /clients is the unified peer surface
#   * /clients ships every alive peer (upload + download + queue +
#     idle), with role-decoded state strings
#   * lazy endpoints' `snapshot_at` reflects per-endpoint fetch time
#     (not the refresher tick boundary)

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_10_refresher_lazy_ondemand_body.XXXXXX)
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

echo "amuleapi 10-refresher-lazy-ondemand smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Refresher has 3 ops/tick now (STAT_REQ + GET_UPDATE + PREFERENCES).
# The first tick still needs ~1 s; sleep 4 covers it comfortably.
sleep 4

# --- 1. /uploads RETIRED → 404. ------------------------------------
#
# The legacy endpoint is gone; /clients replaces it with role-decoded
# state for every peer (upload + download + queue + idle). Consumers
# wanting the legacy view filter client-side by upload_state ==
# "uploading".
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/uploads"
_assert_status 404 "GET /uploads → 404 (endpoint retired)"

# --- 2. /clients shape. --------------------------------------------
#
# Phase 4g unified peer surface. Every alive peer in
# theApp->clientlist surfaces, populated from the EC_TAG_CLIENT
# container inside the GET_UPDATE response.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/clients"
_assert_status 200 "GET /clients → 200"
_assert_json_eq '.clients | type'        array '/clients .clients is array'

CCOUNT=$(printf '%s' "$CURL_BODY" | jq '.clients | length')
echo "    info: $CCOUNT peers cached (populated via GET_UPDATE CLIENT subtree)"

if [ "$CCOUNT" -gt 0 ]; then
	# Per-peer shape. The state enums are wire strings (decoded
	# server-side from US_* / DS_* / IS_* / SO_* / OBST_* codes).
	_assert_json_eq '.clients[0].client_ecid | type'    number   '/clients[0].client_ecid is numeric'
	_assert_json_eq '.clients[0].user_hash | length'    32       '/clients[0].user_hash is 32-char hex'
	_assert_json_eq '.clients[0].upload_state | type'   string   '/clients[0].upload_state is string'
	_assert_json_eq '.clients[0].download_state | type' string   '/clients[0].download_state is string'
	_assert_json_eq '.clients[0].ident_state | type'    string   '/clients[0].ident_state is string'
	_assert_json_eq '.clients[0].software | type'       string   '/clients[0].software is string'
	# #439 peer country: always-present ISO 3166-1 alpha-2 string,
	# empty when GeoIP is off/unresolved (never absent/null).
	_assert_json_eq '.clients[0].country_code | type'   string   '/clients[0].country_code is string (#439)'
	_assert_json_eq '.clients[0].xfer | type'           object   '/clients[0].xfer is object'
	_assert_json_eq '.clients[0].xfer.up_session | type'   number '/clients[0].xfer.up_session is numeric'
	_assert_json_eq '.clients[0].xfer.down_session | type' number '/clients[0].xfer.down_session is numeric'

	# State enum allowlists — any peer's upload_state must be one of
	# the wire strings the walker emits. Catch silent regressions if
	# the enum decoder gets a new US_* value without a mapping.
	BOGUS_US=$(printf '%s' "$CURL_BODY" | jq \
		'[.clients[].upload_state | select(. != "uploading" and . != "queued" and . != "waitcallback" and . != "connecting" and . != "pending" and . != "lowtolowip" and . != "banned" and . != "error" and . != "idle" and . != "unknown")] | length')
	if [ "$BOGUS_US" = "0" ]; then
		_pass "/clients upload_state values are all from the US_* enum mapping"
	else
		_fail "/clients upload_state enum allowlist" \
			"$BOGUS_US peers have an out-of-enum upload_state"
	fi

	BOGUS_DS=$(printf '%s' "$CURL_BODY" | jq \
		'[.clients[].download_state | select(. != "downloading" and . != "onqueue" and . != "connected" and . != "connecting" and . != "waitcallback" and . != "waitcallbackkad" and . != "reqhashset" and . != "noneededparts" and . != "toomanyconns" and . != "toomanyconnskad" and . != "lowtolowip" and . != "banned" and . != "error" and . != "idle" and . != "remotequeuefull" and . != "unknown")] | length')
	if [ "$BOGUS_DS" = "0" ]; then
		_pass "/clients download_state values are all from the DS_* enum mapping"
	else
		_fail "/clients download_state enum allowlist" \
			"$BOGUS_DS peers have an out-of-enum download_state"
	fi
fi

# --- 3. Lazy-fetch endpoints — fresh per-endpoint snapshot_at. -----
#
# Per Phase 4g, /stats/tree, /stats/graphs/{X}, /search/results, and
# /logs/serverinfo no longer ride the refresher tick. Each handler
# drives its own EC roundtrip on first call, coalesced via 1 s TTL.
# The `snapshot_at` field on each reflects the per-endpoint fetch
# time, not the refresher tick.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/tree"
_assert_status 200 "GET /stats/tree → 200 (lazy fetch)"
_assert_json_eq '.nodes | type'        array '/stats/tree .nodes is array'

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/download"
_assert_status 200 "GET /stats/graphs/download → 200 (lazy fetch)"
_assert_json_eq '.graph' download         '/stats/graphs/download reports graph=download'
_assert_json_eq '.unit' bps               '/stats/graphs/download reports unit=bps'
_assert_json_eq '.points | type' array    '/stats/graphs/download .points is array'

# TTL coalescing — two same-endpoint GETs within the 1 s window must
# share the cached backing fetch. Observable via ETag (Phase 7): same
# cached body bytes → same ETag. The per-point timestamps inside
# /stats/graphs are anchored to the cache's `fetched_at`, so they
# stay constant within a cache window; only between fetches do they
# advance.
GRAPH_ETAG_1=$(curl -s -I -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/stats/graphs/download" \
	| sed -n 's/^[Ee][Tt][Aa][Gg]:[[:space:]]*\([^[:cntrl:]]*\).*/\1/p' | head -1)
GRAPH_ETAG_2=$(curl -s -I -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/stats/graphs/download" \
	| sed -n 's/^[Ee][Tt][Aa][Gg]:[[:space:]]*\([^[:cntrl:]]*\).*/\1/p' | head -1)
if [ "$GRAPH_ETAG_1" = "$GRAPH_ETAG_2" ] && [ -n "$GRAPH_ETAG_1" ]; then
	_pass "/stats/graphs/download back-to-back share the same fetch (1 s TTL coalescing; ETag stable: $GRAPH_ETAG_1)"
else
	_fail "/stats/graphs TTL coalescing" \
		"first ETag=$GRAPH_ETAG_1, second=$GRAPH_ETAG_2 — expected identical within the 1 s window"
fi

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/stats/graphs/bogus"
_assert_status 404 "GET /stats/graphs/bogus → 404 (still validated)"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/search/results"
_assert_status 200 "GET /search/results → 200 (lazy fetch)"
_assert_json_eq '.results | type' array '/search/results .results is array'

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/logs/serverinfo"
_assert_status 200 "GET /logs/serverinfo → 200 (lazy fetch)"
_assert_json_eq '.text | type' string '/logs/serverinfo .text is string'
_assert_json_eq '.total_bytes | type' number '/logs/serverinfo .total_bytes is numeric'

# --- 4. Per-tick endpoints still fresh from the refresher. ---------
#
# Sanity check the trio that stays on the refresher path —
# downloads / shared / servers / clients / status / kad — they all
# pull `snapshot_at` from CState::SnapshotAt which marks tick
# completion.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/status"
_assert_status 200 "GET /status → 200 (still per-tick)"
_assert_json_eq '.ed2k.state | type' string '/status.ed2k.state populated'

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads"
_assert_status 200 "GET /downloads → 200 (still per-tick)"
_assert_json_eq '.downloads | type' array '/downloads .downloads is array'

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
_assert_status 200 "GET /shared → 200 (still per-tick)"
_assert_json_eq '.shared | type' array '/shared .shared is array'

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/servers"
_assert_status 200 "GET /servers → 200 (still per-tick)"
_assert_json_eq '.servers | type' array '/servers .servers is array'

# --- 5. Method gate on /clients. -----------------------------------
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/clients"
_assert_status 405 "DELETE /clients → 405"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
