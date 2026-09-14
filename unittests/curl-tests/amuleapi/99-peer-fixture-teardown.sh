#!/usr/bin/env bash
#
# amuleapi 99-peer-fixture-teardown - undo 00-peer-fixture.
#
# The fixture is the one thing in the run that is a real file off the real
# network. Left behind it keeps downloading after the suite exits.
#
# Three end states, because which one occurs depends on transfer speed and on
# whether 13's blanket clear_completed ran while the fixture was finished:
#
#   still downloading  -> DELETE /downloads/{hash} (drops the partfile too)
#   completed, listed  -> clear_completed {hash}, then unlink from Incoming
#   completed, swept   -> no queue entry at all, but the file is in Incoming
#
# The last is why the handoff records the name: after a sweep the API can no
# longer say what the download was called.
#
# Usage:
#   ./99-peer-fixture-teardown.sh     # via run-all.sh, as the final phase
#
# Environment:
#   HOST=localhost:4713               amuleapi endpoint
#   ADMIN_PASS=adminpass              admin credential
#   PEER_FIXTURE_STATE=/tmp/amuleapi-peer-fixture   handoff file from 00
#
# Exits non-zero only if a provisioned fixture could not be removed. That IS a
# failure: a run that leaves a live transfer behind has changed the machine it
# was testing on.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
API="$HOST/api/v1"
ADMIN_PASS=${ADMIN_PASS:-adminpass}
STATE=${PEER_FIXTURE_STATE:-/tmp/amuleapi-peer-fixture}

FAIL_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_99_peer_fixture_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_ok()   { echo "  OK    $1"; }
_fail() {
	FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	local resp
	resp=$(curl -s --max-time 15 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
}

echo "amuleapi 99-peer-fixture-teardown @ $HOST"

if [ ! -f "$STATE" ]; then
	echo "  nothing to undo (no $STATE -- 00-peer-fixture did not provision)"
	echo "99-peer-fixture-teardown: nothing to do"
	exit 0
fi

# Parsed by hand rather than sourced so a mangled file cannot execute anything.
FIX_HASH=$(sed -n 's/^hash=//p' "$STATE" | head -1)
FIX_NAME=$(sed -n 's/^name=//p' "$STATE" | head -1)
[ -n "$FIX_HASH" ] || _die "$STATE exists but carries no hash= line"
echo "  fixture: $FIX_HASH ($FIX_NAME)"

_curl -X POST -H 'Content-Type: application/json' \
	-d "{\"username\":\"admin\",\"password\":\"$ADMIN_PASS\"}" \
	"$API/auth/login?include_token=true"
[ "$CURL_STATUS" = "200" ] || _die "admin login failed (HTTP $CURL_STATUS)"
TOKEN=$(printf '%s' "$CURL_BODY" | jq -r '.token // empty')
[ -n "$TOKEN" ] || _die "admin login returned no token"
AUTH="Authorization: Bearer $TOKEN"

# Resolved first so a preferences problem cannot look like a delete problem.
_curl -H "$AUTH" "$API/preferences"
INCOMING=$(printf '%s' "$CURL_BODY" | jq -r '.directories.incoming_path // empty')

# status=all matters: the default list hides completed entries, and a completed
# fixture is exactly the case that needs the file unlinked.
_curl -H "$AUTH" "$API/downloads?status=all"
STATUS=$(printf '%s' "$CURL_BODY" \
	| jq -r --arg h "$FIX_HASH" '[.downloads[] | select(.hash == $h) | .status][0] // "absent"')
# amuled saves under a different name when one already exists, so trust it over
# the search result's name.
LIVE_NAME=$(printf '%s' "$CURL_BODY" \
	| jq -r --arg h "$FIX_HASH" '[.downloads[] | select(.hash == $h) | .name][0] // empty')
[ -n "$LIVE_NAME" ] && FIX_NAME=$LIVE_NAME
echo "  queue status: $STATUS"

case "$STATUS" in
absent)
	_ok "no queue entry to remove"
	;;
completed)
	_curl -X POST -H "$AUTH" -H 'Content-Type: application/json' \
		-d "{\"hash\":\"$FIX_HASH\"}" "$API/downloads_clear_completed"
	if [ "$CURL_STATUS" = "200" ]; then
		_ok "cleared completed fixture from the queue"
	else
		_fail "clear_completed {hash:fixture}" \
			"expected HTTP 200, got $CURL_STATUS" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
	;;
*)
	_curl -X DELETE -H "$AUTH" "$API/downloads/$FIX_HASH"
	if [ "$CURL_STATUS" = "204" ]; then
		_ok "deleted the in-progress fixture download"
	else
		_fail "DELETE /downloads/{fixture}" \
			"expected HTTP 204, got $CURL_STATUS" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
	# 204 while the entry survives is the bug that leaves a transfer running.
	_curl -H "$AUTH" "$API/downloads?status=all"
	GONE=$(printf '%s' "$CURL_BODY" \
		| jq -r --arg h "$FIX_HASH" '[.downloads[] | select(.hash == $h)] | length')
	if [ "${GONE:-1}" = "0" ]; then
		_ok "fixture no longer in /downloads?status=all"
	else
		_fail "fixture still queued after DELETE" \
			"a live transfer would outlive this suite run"
	fi
	;;
esac

# Unlink exactly one path, built from the daemon's own incoming_path and the
# entry's own name, and only when it is a regular file. basename defends
# against a name carrying a separator: the cost of being wrong is an unlink
# outside Incoming.
if [ -n "$INCOMING" ] && [ -n "$FIX_NAME" ]; then
	CANDIDATE="$INCOMING/$(basename "$FIX_NAME")"
	if [ -f "$CANDIDATE" ]; then
		if rm -f "$CANDIDATE"; then
			_ok "removed completed fixture file $CANDIDATE"
			# amuled shares a completed download and unlinking does not
			# retract that, so the share survives pointing at a path
			# that is gone. 17-shared-priority-patch renames shared[0]
			# and failed three assertions on it until 36's reload
			# cleaned up, nineteen phases too late.
			_curl -X POST -H "$AUTH" "$API/shared_reload"
			case "$CURL_STATUS" in
			200 | 202) _ok "reloaded shares so amuled drops the stale entry" ;;
			*) _fail "POST /shared_reload" \
				"expected HTTP 202, got $CURL_STATUS" \
				"a share pointing at the deleted file survives into the next run" ;;
			esac
		else
			_fail "could not remove $CANDIDATE" \
				"the fixture file is still in the incoming directory"
		fi
	else
		_ok "no fixture file in $INCOMING (download never completed)"
	fi
else
	echo "  note: incoming_path unavailable; cannot check for a completed file"
fi

rm -f "$STATE"

echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "99-peer-fixture-teardown: fixture removed"
	exit 0
fi
echo "99-peer-fixture-teardown: $FAIL_COUNT cleanup step(s) FAILED"
exit 1
