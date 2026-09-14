#!/usr/bin/env bash
#
# amuleapi 00-peer-fixture - queue a real download so later phases have a peer.
#
# The downloads the suite makes for itself use fabricated ed2k hashes, which
# never acquire a source, so every source-dependent check in 04, 33 and 43
# skipped unless someone provisioned a transfer by hand. This searches the live
# network and queues the most-sourced hit; 99-peer-fixture-teardown removes it.
#
# Never fatal: no network is an ordinary condition, and failing the suite for it
# would make the suite unusable offline. It must not fail QUIETLY, so giving up
# prints a banner naming the reason.
#
# Usage:
#   ./00-peer-fixture.sh              # via run-all.sh, which supplies the daemon
#
# Environment:
#   HOST=localhost:4713               amuleapi endpoint
#   ADMIN_PASS=adminpass              admin credential
#   PEER_FIXTURE_QUERY=ubuntu         keyword to search for
#   PEER_FIXTURE_SEARCH_WAIT=45       seconds to let results accumulate
#   PEER_FIXTURE_SOURCE_WAIT=90       seconds to wait for a source to attach
#   PEER_FIXTURE_MIN_SIZE=268435456   prefer hits at least this many bytes
#   PEER_FIXTURE_STATE=/tmp/amuleapi-peer-fixture   handoff file for teardown
#
# Exits 0 provisioned or not; 2 only on a bring-up error (no daemon, no login).

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
API="$HOST/api/v1"
ADMIN_PASS=${ADMIN_PASS:-adminpass}
QUERY=${PEER_FIXTURE_QUERY:-ubuntu}
SEARCH_WAIT=${PEER_FIXTURE_SEARCH_WAIT:-45}
SOURCE_WAIT=${PEER_FIXTURE_SOURCE_WAIT:-90}
STATE=${PEER_FIXTURE_STATE:-/tmp/amuleapi-peer-fixture}

# Measured: the most-sourced "ubuntu" hit overall was a 3.3 MB PDF that finished
# 30 s after being queued, and a completed download drops its sources - so 04,
# 33 and 43 ten minutes later saw the empty peer list this fixture exists to
# prevent. Prefer hits too big for a run to finish.
MIN_SIZE=${PEER_FIXTURE_MIN_SIZE:-268435456}

# 12/13/22/23/24/26 add and delete this hash as their own disposable fixture.
SUITE_TEST_HASH=0031c9cba65c50dd2015c184b2ca2c88

CURL_BODY_FILE=$(mktemp -t amuleapi_00_peer_fixture_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT
CURL_STATUS=""
CURL_BODY=""

# Governs whether giving up may delete the handoff: once set there is a real
# transfer to tear down, and dropping the handoff would orphan it.
QUEUED=0

# Unlike the assertion phases, a transport error is not fatal here.
_curl() {
	local resp
	resp=$(curl -s --max-time 15 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") || {
		CURL_STATUS="000"
		CURL_BODY=""
		return 0
	}
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
}

# Greppable on purpose: this is what to look for when the peer checks skip.
_not_provisioned() {
	[ "$QUEUED" = "0" ] && rm -f "$STATE"
	echo
	echo "########################################################################"
	echo "##  PEER FIXTURE NOT PROVISIONED"
	echo "##  reason: $1"
	echo "##"
	echo "##  The source-dependent checks in 04-read-downloads-shared,"
	echo "##  33-known-clients and 43-client-protocol-extensions will SKIP."
	echo "##  That is not a failure, but it is less coverage than a full run."
	echo "########################################################################"
	echo
	exit 0
}

_die() { echo "FATAL: $*" >&2; exit 2; }

echo "amuleapi 00-peer-fixture @ $HOST (query: $QUERY)"

# Reaches jq as --argjson, where a non-numeric value would read as "no result
# had a source" and send the operator looking at the network.
case "$MIN_SIZE" in
'' | *[!0-9]*) _die "PEER_FIXTURE_MIN_SIZE must be a plain byte count, got '$MIN_SIZE'" ;;
esac

# A handoff from an interrupted run would make teardown delete someone else's
# download.
rm -f "$STATE"

_curl -X POST -H 'Content-Type: application/json' \
	-d "{\"username\":\"admin\",\"password\":\"$ADMIN_PASS\"}" \
	"$API/auth/login?include_token=true"
[ "$CURL_STATUS" = "200" ] || _die "admin login failed (HTTP $CURL_STATUS)"
TOKEN=$(printf '%s' "$CURL_BODY" | jq -r '.token // empty')
[ -n "$TOKEN" ] || _die "admin login returned no token"
AUTH="Authorization: Bearer $TOKEN"

# Cheaper than a 45 s wait that ends in an empty result set.
_curl -H "$AUTH" "$API/status"
ED2K=$(printf '%s' "$CURL_BODY" | jq -r '.ed2k.state // "unknown"')
KAD=$(printf '%s' "$CURL_BODY" | jq -r '.kad.state // "unknown"')
echo "    ed2k=$ED2K kad=$KAD"
if [ "$ED2K" != "connected" ] && [ "$KAD" != "connected" ]; then
	_not_provisioned "neither ed2k nor Kad is connected, so no search can return sources"
fi

_curl -X POST -H "$AUTH" -H 'Content-Type: application/json' \
	-d "{\"query\":\"$QUERY\"}" "$API/search"
case "$CURL_STATUS" in
200 | 202) ;;
*) _not_provisioned "POST /search returned HTTP $CURL_STATUS" ;;
esac
SEARCH_ID=$(printf '%s' "$CURL_BODY" | jq -r '.search_id // empty')
[ -n "$SEARCH_ID" ] || _not_provisioned "POST /search returned no search_id"
echo "    search_id=$SEARCH_ID; collecting results for up to ${SEARCH_WAIT}s"

# The result floor is a second exit for a server that keeps a search open long
# after it has sent everything it has.
_elapsed=0
while [ "$_elapsed" -lt "$SEARCH_WAIT" ]; do
	sleep 5
	_elapsed=$((_elapsed + 5))
	_curl -H "$AUTH" "$API/search/$SEARCH_ID/results"
	[ "$CURL_STATUS" = "200" ] || continue
	SEARCH_STATE=$(printf '%s' "$CURL_BODY" | jq -r '.progress.state // "unknown"')
	SEARCH_HITS=$(printf '%s' "$CURL_BODY" | jq -r '.results | length')
	if [ "$SEARCH_STATE" = "finished" ] || [ "${SEARCH_HITS:-0}" -ge 50 ]; then
		break
	fi
done

# Most sources wins within the big tier, falling back to the whole field.
# already_downloaded hits would complete against the local library and attach
# nothing. Name goes last because it is the only field that can hold a space.
BEST_TIER=""
BEST_HASH=""
BEST_SOURCES=""
BEST_SIZE=""
BEST_NAME=""
read -r BEST_TIER BEST_HASH BEST_SOURCES BEST_SIZE BEST_NAME <<EOF
$(printf '%s' "$CURL_BODY" | jq -r --arg skip "$SUITE_TEST_HASH" --argjson min "$MIN_SIZE" '
	[ .results[]?
	  | select(.already_downloaded != true)
	  | select((.hash // "") != $skip)
	  | select((.sources.total // 0) > 0)
	] as $usable
	| ( [ $usable[] | select((.size_bytes // 0) >= $min) ]
	    | sort_by(-(.sources.total // 0)) | .[0] ) as $big
	| ( $usable | sort_by(-(.sources.total // 0)) | .[0] ) as $any
	| ($big // $any)
	| if . == null then ""
	  else "\(if (.size_bytes // 0) >= $min then "large" else "small" end) \(.hash) \(.sources.total) \(.size_bytes) \(.name)"
	  end')
EOF

# amuled caps concurrent searches; do not charge the rest of the suite for ours.
_curl -X DELETE -H "$AUTH" "$API/search/$SEARCH_ID"

[ -n "$BEST_HASH" ] \
	|| _not_provisioned "search for '$QUERY' returned no usable result with a live source"
echo "    best hit: $BEST_SOURCES sources, $BEST_SIZE bytes -- $BEST_NAME"
if [ "$BEST_TIER" = "small" ]; then
	echo "    note: no hit of at least $MIN_SIZE bytes had a source; this one may"
	echo "          complete mid-run, and its peers drop when it does"
fi

_curl -X POST -H "$AUTH" -H 'Content-Type: application/json' -d '{}' \
	"$API/search/results/$BEST_HASH/download"
case "$CURL_STATUS" in
200 | 202) ;;
*) _not_provisioned "promoting $BEST_HASH to a download returned HTTP $CURL_STATUS" ;;
esac

# Written before anything is confirmed: a download may exist from the request
# above onward. A handoff naming one that was never created is harmless, so the
# asymmetry runs the safe way.
printf 'hash=%s\nname=%s\n' "$BEST_HASH" "$BEST_NAME" > "$STATE"

# The status above proves nothing. Measured: this endpoint answers 202 with an
# empty body for a hash in no search at all, and queues nothing - it hands the
# request to amuled over EC without waiting for a verdict.
APPEARED=0
_elapsed=0
while [ "$_elapsed" -lt 20 ]; do
	_curl -H "$AUTH" "$API/downloads?status=all"
	if [ "$CURL_STATUS" = "200" ] && printf '%s' "$CURL_BODY" \
	   | jq -e --arg h "$BEST_HASH" '.downloads[] | select(.hash == $h)' >/dev/null 2>&1; then
		APPEARED=1
		break
	fi
	sleep 2
	_elapsed=$((_elapsed + 2))
done
[ "$APPEARED" = "1" ] \
	|| _not_provisioned "the daemon accepted the download request for $BEST_HASH but the download never appeared in the queue"
QUEUED=1
echo "    queued $BEST_HASH; waiting up to ${SOURCE_WAIT}s for a source"

_elapsed=0
SOURCES=0
while [ "$_elapsed" -lt "$SOURCE_WAIT" ]; do
	sleep 5
	_elapsed=$((_elapsed + 5))
	_curl -H "$AUTH" "$API/downloads/$BEST_HASH"
	[ "$CURL_STATUS" = "200" ] || continue
	SOURCES=$(printf '%s' "$CURL_BODY" | jq -r '.sources.total // 0')
	[ "${SOURCES:-0}" -gt 0 ] && break
done

if [ "${SOURCES:-0}" -gt 0 ]; then
	echo "    $SOURCES source(s) attached after ${_elapsed}s"
	echo "00-peer-fixture: provisioned"
	exit 0
fi

# Still queued, and teardown still removes it; only the peer checks lose out.
_not_provisioned "the download was queued but no source attached within ${SOURCE_WAIT}s"
