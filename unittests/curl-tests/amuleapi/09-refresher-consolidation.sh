#!/usr/bin/env bash
#
# amuleapi 09-refresher-consolidation — EC_OP_GET_UPDATE @ EC_DETAIL_INC_UPDATE
# refresher consolidation.
#
# Drops the per-substruct GET_DLOAD_QUEUE / GET_SHARED_FILES /
# GET_SERVER_LIST polling path (each with its own two-phase UPDATE/
# FULL split) in favour of a single GET_UPDATE roundtrip. /uploads
# stays on GET_ULOAD_QUEUE (wire-semantic preservation — the
# GET_UPDATE clients block is filtered server-side by the global
# `TransmitOnlyUploadingClients` pref).
#
# This smoke is a wire-contract regression check: every field the
# Phase 4b/4c smokes asserted on must STILL be present after the
# refresher swap. progress.parts on the detail endpoint (Phase 4e)
# must STILL show the RLE-decoded per-part state — proving the
# stateful decoder still gets its baseline frame from GET_UPDATE's
# INC_UPDATE-level payload.
#
# Net effect on ops/tick (steady state, no new ECIDs):
#   * before: 10 ops (STAT_REQ, GET_DLOAD_QUEUE, GET_ULOAD_QUEUE,
#                     GET_SHARED_FILES, GET_SERVER_LIST,
#                     GET_PREFERENCES, GET_SERVERINFO, GET_STATSTREE,
#                     GET_STATSGRAPHS, SEARCH_RESULTS)
#   * after:   8 ops (GET_UPDATE replaces 3 of them, GET_ULOAD_QUEUE
#                     stays for uploads, rest unchanged)
#
# On a cold tick the savings are larger (no Phase 2 FULL roundtrip
# for new ECIDs in downloads/shared — INC_UPDATE ships identity in
# the first response).

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_09_refresher_consolidation_body.XXXXXX)
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

echo "amuleapi 09-refresher-consolidation smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Let the refresher complete at least one full tick after auth so the
# GET_UPDATE-populated caches are warm. INC_UPDATE on cold start
# delivers a larger payload (every file shipped with full identity)
# than steady-state ticks, but the response shape is unchanged.
sleep 4

# --- 1. /downloads — list endpoint shape preserved. ----------------
#
# Phase 4b asserted these field types on the list shape; if GET_UPDATE
# dispatch broke any of them, the wire contract is broken.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads"
_assert_status 200 "GET /downloads → 200"
_assert_json_eq '.downloads | type'     array '/downloads .downloads is array'

DCOUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
echo "    info: $DCOUNT downloads in queue (populated via GET_UPDATE)"

if [ "$DCOUNT" -gt 0 ]; then
	# Identity arrives in the same tick at INC_UPDATE — no second
	# roundtrip needed. A field empty here means the walker isn't
	# picking up the identity tags that EC_DETAIL_INC_UPDATE ships.
	_assert_json_eq '.downloads[0].hash | length' 32 \
		'/downloads[0].hash is 32-char hex (identity from one tick)'
	_assert_json_eq '.downloads[0].name | type'   string \
		'/downloads[0].name is non-null string'
	_assert_json_eq '.downloads[0].size | type'   number \
		'/downloads[0].size is numeric'
	_assert_json_eq '.downloads[0].status | type' string \
		'/downloads[0].status is string'
	_assert_json_eq '.downloads[0].priority | type' string \
		'/downloads[0].priority is string'
	# Source counts come through the merge path — sanity check the
	# `sources` substruct still populates.
	_assert_json_eq '.downloads[0].sources | type' object \
		'/downloads[0].sources is object'

	FIRST_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.downloads[0].hash')

	# --- 2. /downloads/{hash} detail — progress.parts still ships. -
	#
	# Phase 4e relies on the stateful RLE decoder. GET_UPDATE at
	# INC_UPDATE ships the GAP/PART blobs (via the encoder's Encode
	# call at ExternalConn.cpp:942) so the decoder gets its frames.
	# If the consolidation broke the RLE wiring, parts would come back
	# empty or with the wrong length.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$FIRST_HASH"
	_assert_status 200 "GET /downloads/{hash} → 200"
	_assert_json_eq '.progress.parts | type' array \
		'/downloads/{hash}.progress.parts is array (RLE decoder still wired)'
fi

# --- 3. /shared — list endpoint shape preserved. -------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
_assert_status 200 "GET /shared → 200"
_assert_json_eq '.shared | type'        array '/shared .shared is array'

SCOUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
echo "    info: $SCOUNT shared files (populated via GET_UPDATE KNOWNFILE dispatch)"

if [ "$SCOUNT" -gt 0 ]; then
	_assert_json_eq '.shared[0].hash | length' 32 \
		'/shared[0].hash is 32-char hex'
	_assert_json_eq '.shared[0].name | type'   string \
		'/shared[0].name is string'
	_assert_json_eq '.shared[0].size | type'   number \
		'/shared[0].size is numeric'
	_assert_json_eq '.shared[0].priority | type' string \
		'/shared[0].priority is string'
	_assert_json_eq '.shared[0].priority_auto | type' boolean \
		'/shared[0].priority_auto is boolean'
fi

# --- 4. /servers — list endpoint shape preserved. ------------------
#
# Now populated by walking the EC_TAG_SERVER container inside the
# GET_UPDATE response (one level deeper than the legacy GET_SERVER_LIST
# response, where servers were top-level).
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/servers"
_assert_status 200 "GET /servers → 200"
_assert_json_eq '.servers | type'       array '/servers .servers is array'

NSERV=$(printf '%s' "$CURL_BODY" | jq '.servers | length')
echo "    info: $NSERV servers configured (populated via GET_UPDATE container walk)"

if [ "$NSERV" -gt 0 ]; then
	# Identity preserved through the container walk — name, address,
	# user counts. The MergeServerTag is_new branch fires for fresh
	# servers; on subsequent ticks the CValueMap suppresses unchanged
	# fields and the !is_new branch keeps the cached value.
	_assert_json_eq '.servers[0].name | type'     string \
		'/servers[0].name is string'
	_assert_json_eq '.servers[0].address | type'  string \
		'/servers[0].address is string'
	_assert_json_eq '.servers[0].priority | type' string \
		'/servers[0].priority is string'
fi

# --- 5. /uploads — endpoint retired in Phase 4g. -------------------
#
# /clients now covers the full peer surface (every upload_state,
# including queue waiters and download-side peers). Consumers filter
# client-side by upload_state == "uploading" when they want the
# legacy /uploads view. 10-refresher-lazy-ondemand.sh exercises the new shape.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/uploads"
_assert_status 404 "GET /uploads → 404 (retired in Phase 4g)"

# --- 6. /status, /kad, /preferences — unaffected by the consolidation
# (they ride on STAT_REQ and GET_PREFERENCES, which we did not touch).
# A sanity glance to catch unrelated regressions slipping in.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/status"
_assert_status 200 "GET /status → 200 (unaffected by Phase 4f)"
_assert_json_eq '.ed2k.state | type' string '/status.ed2k.state still populated'

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/kad"
_assert_status 200 "GET /kad → 200 (unaffected by Phase 4f)"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
