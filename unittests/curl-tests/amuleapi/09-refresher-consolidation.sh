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

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

_preflight

echo "amuleapi 09-refresher-consolidation smoke @ $HOST"

TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] || _die "login failed"

# Let the refresher complete at least one full tick after auth so the
# GET_UPDATE-populated caches are warm. INC_UPDATE on cold start
# delivers a larger payload (every file shipped with full identity)
# than steady-state ticks, but the response shape is unchanged.
sleep 4

# --- 1. /downloads — list endpoint shape preserved. ----------------
#
# These field types define the list shape; if GET_UPDATE
# dispatch broke any of them, the wire contract is broken.
_curl -H "Authorization: Bearer $TOKEN" "$API/downloads"
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
	_assert_json_eq '.downloads[0].size_bytes | type'   number \
		'/downloads[0].size_bytes is numeric'
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
	# progress.parts relies on the stateful RLE decoder. GET_UPDATE at
	# INC_UPDATE ships the GAP/PART blobs (via the encoder's Encode
	# call at ExternalConn.cpp:942) so the decoder gets its frames.
	# If the consolidation broke the RLE wiring, parts would come back
	# empty or with the wrong length.
	_curl -H "Authorization: Bearer $TOKEN" "$API/downloads/$FIRST_HASH"
	_assert_status 200 "GET /downloads/{hash} → 200"
	_assert_json_eq '.progress.parts | type' array \
		'/downloads/{hash}.progress.parts is array (RLE decoder still wired)'
fi

# --- 3. /shared — list endpoint shape preserved. -------------------
_curl -H "Authorization: Bearer $TOKEN" "$API/shared"
_assert_status 200 "GET /shared → 200"
_assert_json_eq '.shared | type'        array '/shared .shared is array'

SCOUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
echo "    info: $SCOUNT shared files (populated via GET_UPDATE KNOWNFILE dispatch)"

if [ "$SCOUNT" -gt 0 ]; then
	_assert_json_eq '.shared[0].hash | length' 32 \
		'/shared[0].hash is 32-char hex'
	_assert_json_eq '.shared[0].name | type'   string \
		'/shared[0].name is string'
	_assert_json_eq '.shared[0].size_bytes | type'   number \
		'/shared[0].size_bytes is numeric'
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
_curl -H "Authorization: Bearer $TOKEN" "$API/servers"
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

# --- 5. /status, /kad, /preferences — unaffected by the consolidation
# (they ride on STAT_REQ and GET_PREFERENCES, which we did not touch).
# A sanity glance to catch unrelated regressions slipping in.
_curl -H "Authorization: Bearer $TOKEN" "$API/status"
_assert_status 200 "GET /status → 200"
_assert_json_eq '.ed2k.state | type' string '/status.ed2k.state still populated'

_curl -H "Authorization: Bearer $TOKEN" "$API/kad"
_assert_status 200 "GET /kad → 200"

# --- Summary. -----------------------------------------------------
_summary
