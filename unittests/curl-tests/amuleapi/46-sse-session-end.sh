#!/usr/bin/env bash
#
# amuleapi 46-sse-session-end — an /events stream lives only as long as
# the session that opened it, and guests cannot take every stream slot.
#
# Wire contract:
#   * The stream re-checks its token each time it wakes (an event or the
#     15 s heartbeat). A logout, a credential change (guest access
#     disabled included) or expiry closes it, with no event after that.
#   * Of the 32 stream slots, 4 are kept for admin streams: the 29th
#     concurrent guest stream gets 503 too_many_streams, an admin stream
#     still opens.

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

WORK=$(mktemp -d -t amuleapi_46_sse_session_end.XXXXXX)
PIDS=()
cleanup() {
	local p
	for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
	rm -rf "$WORK"
}
_on_exit cleanup

# Opens a stream in the background as `$1` (token), body to $WORK/$2.
# Sets STREAM_PID.
_open_stream() {
	curl -s -N -m 90 -o "$WORK/$2" -D "$WORK/$2.head" \
		-H "Authorization: Bearer $1" "$API/events" 2>/dev/null &
	STREAM_PID=$!
	PIDS+=("$STREAM_PID")
}

# Waits up to `$2` s for the stream body `$1` to show `: connected`.
_wait_connected() {
	local _
	for _ in $(seq 1 $(($2 * 4))); do
		grep -q '^: connected' "$WORK/$1" 2>/dev/null && return 0
		sleep 0.25
	done
	return 1
}

# Waits up to `$2` s for process `$1` to exit.
_wait_exit() {
	local _
	for _ in $(seq 1 $(($2 * 4))); do
		kill -0 "$1" 2>/dev/null || return 0
		sleep 0.25
	done
	return 1
}

_preflight

echo "amuleapi 46-sse-session-end smoke @ $HOST"

# --- 1. Logout ends the stream. -------------------------------------
ADMIN_TOKEN=$(_login "$ADMIN_PASS")
[ -n "$ADMIN_TOKEN" ] || _die "admin login failed"

_open_stream "$ADMIN_TOKEN" logout
LOGOUT_PID=$STREAM_PID
_wait_connected logout 5 || _die "admin stream did not open"
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
	-H "Authorization: Bearer $ADMIN_TOKEN" "$API/auth/logout")
[ "$CODE" = "204" ] || _die "logout returned $CODE"
AT_LOGOUT=$(wc -c < "$WORK/logout")

# The stream notices at its next wake, the heartbeat at the latest.
if _wait_exit "$LOGOUT_PID" 20; then
	_pass "logout closes the token's open stream"
else
	_fail "logout closes the token's open stream" "stream still open 20 s after logout"
fi
if tail -c +$((AT_LOGOUT + 1)) "$WORK/logout" | grep -q '^event:'; then
	_fail "no event after logout" "$(tail -c +$((AT_LOGOUT + 1)) "$WORK/logout" | grep '^event:' | head -3)"
else
	_pass "no event after logout"
fi

# --- 2. Guests cannot take the admin's reserved slots. --------------
GUEST_TOKEN=$(_login "$GUEST_PASS")
[ -n "$GUEST_TOKEN" ] || _die "guest login failed"
ADMIN_TOKEN=$(_login "$ADMIN_PASS")
[ -n "$ADMIN_TOKEN" ] || _die "admin login failed"

GUEST_PIDS=()
for n in $(seq 1 28); do
	_open_stream "$GUEST_TOKEN" "guest$n"
	GUEST_PIDS+=("$STREAM_PID")
done
OPEN=0
for n in $(seq 1 28); do
	_wait_connected "guest$n" 5 && OPEN=$((OPEN + 1))
done
if [ "$OPEN" -eq 28 ]; then
	_pass "28 concurrent guest streams open"
else
	_fail "28 concurrent guest streams open" "only $OPEN opened"
fi

CODE=$(curl -s -m 5 -o "$WORK/guest29" -w '%{http_code}' \
	-H "Authorization: Bearer $GUEST_TOKEN" "$API/events")
if [ "$CODE" = "503" ] && jq -e '.error.code == "too_many_streams"' "$WORK/guest29" >/dev/null 2>&1; then
	_pass "29th guest stream → 503 too_many_streams"
else
	_fail "29th guest stream → 503 too_many_streams" "got HTTP $CODE: $(head -c 200 "$WORK/guest29")"
fi

_open_stream "$ADMIN_TOKEN" admin
ADMIN_PID=$STREAM_PID
if _wait_connected admin 5; then
	_pass "admin stream opens while guests hold 28 slots"
else
	_fail "admin stream opens while guests hold 28 slots" \
		"head: $(head -1 "$WORK/admin.head" 2>/dev/null)"
fi

# --- 3. Disabling guest access ends the guest streams. --------------
# The credential cutoff has one-second granularity: let the tokens age.
sleep 1.5
CODE=$(curl -s -o /dev/null -w '%{http_code}' -X PATCH \
	-H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"current_password\":\"$ADMIN_PASS\",\"guest_access_enabled\":false}" \
	"$API/auth/passwords")
[ "$CODE" = "200" ] || _die "PATCH /auth/passwords returned $CODE"

CLOSED=0
for p in "${GUEST_PIDS[@]}"; do
	_wait_exit "$p" 20 && CLOSED=$((CLOSED + 1))
done
if [ "$CLOSED" -eq 28 ]; then
	_pass "disabling guest access closes every guest stream"
else
	_fail "disabling guest access closes every guest stream" "$CLOSED of 28 closed"
fi

# A credential change ends every session opened before it, the admin's too.
if _wait_exit "$ADMIN_PID" 20; then
	_pass "the credential change also closes the admin's older stream"
else
	_fail "the credential change also closes the admin's older stream" "still open"
fi

# --- Summary. -----------------------------------------------------
_summary
