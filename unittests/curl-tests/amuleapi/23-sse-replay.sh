#!/usr/bin/env bash
#
# amuleapi 23-sse-replay — Last-Event-ID replay.
#
# Wire contract for Phase 8c:
#   * SSE clients reconnecting with a `Last-Event-ID: <N>` request
#     header are sent every event with id > N that's still in the
#     bus's ring (default 16384 slots, operator-tunable via
#     [Streaming]/EventBusRingCapacity) before the drain loop starts.
#   * Replay is monotonic and gap-free: the first id seen after
#     reconnect is N+1 (provided that id is still in the ring).
#   * If the requested Last-Event-ID is older than the bus's oldest
#     retained event, the daemon emits a `: replay-gap` SSE comment
#     and clamps the start to oldest-1. (Phase 8d upgrades this to
#     a typed `resync` event.)
#   * If the requested Last-Event-ID is higher than the bus's newest
#     id (e.g. stale id from a prior daemon process), the daemon
#     clamps to NewestId and proceeds — no infinite wait.
#   * Absent or unparseable header → behaviour identical to 8b
#     (start from NewestId, no replay). Covered by 22-sse-diff-emission.sh
#     already, not retested here.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"

FAIL_COUNT=0
TEST_COUNT=0

SSE1=$(mktemp -t amuleapi_23_sse_replay_1.XXXXXX)
SSE2=$(mktemp -t amuleapi_23_sse_replay_2.XXXXXX)
trap 'rm -f "$SSE1" "$SSE2"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 23-sse-replay smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

sleep 4

# Make sure the ISO isn't lingering from a prior smoke.
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null 2>&1 || true
sleep 2

# --- 1. Capture a Last-Event-ID, disconnect, mutate, reconnect. ---
#
# Open SSE1, trigger POST /downloads, capture the last id seen,
# disconnect, then trigger DELETE while no one is subscribed.
# Reconnect SSE2 with `Last-Event-ID: <captured>` and verify the
# `download_removed` that fired during the gap is replayed.

: > "$SSE1"
(curl -s -m 8 -N -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE1" 2>&1) &
PID1=$!
sleep 1
echo "    info: POST Ubuntu ISO..."
curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" \
	"$HOST/api/v0/downloads" > /dev/null
# Wait for the download_added so we have at least one ratcheted id.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
         21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40; do
	if grep -q "^event: download_added$" "$SSE1"; then break; fi
	sleep 0.2
done
sleep 1
kill $PID1 2>/dev/null
wait $PID1 2>/dev/null

LAST_ID=$(grep "^id: " "$SSE1" | tail -1 | sed 's/^id: //')
if [ -n "$LAST_ID" ] && [ "$LAST_ID" -gt 0 ] 2>/dev/null; then
	_pass "Captured Last-Event-ID from first subscriber ($LAST_ID)"
else
	_fail "Capture Last-Event-ID" \
		"no id line in first stream; sample: $(head -20 "$SSE1")"
	echo
	echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
	exit 1
fi

# Trigger one more mutation while NO subscriber is open. This event
# must be replayed to SSE2 once it reconnects with Last-Event-ID.
echo "    info: DELETE while disconnected..."
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null
sleep 4

# --- 2. Reconnect with Last-Event-ID, verify the gap is replayed. -
: > "$SSE2"
(curl -s -m 6 -N \
	-H "Last-Event-ID: $LAST_ID" \
	-H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE2" 2>&1) &
PID2=$!
# Replayed events should land essentially immediately; poll for the
# expected download_removed up to a few seconds.
GOT_REMOVED=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
         21 22 23 24 25; do
	if grep -q "^event: download_removed$" "$SSE2"; then
		GOT_REMOVED=$(grep -A2 "^event: download_removed$" "$SSE2" \
			| grep "^data: " | grep -F "$TEST_HASH" | head -1)
		if [ -n "$GOT_REMOVED" ]; then break; fi
	fi
	sleep 0.2
done
kill $PID2 2>/dev/null
wait $PID2 2>/dev/null

if [ -n "$GOT_REMOVED" ]; then
	_pass "download_removed fired during the gap is replayed on reconnect"
else
	_fail "Replay of gap event" \
		"no download_removed for $TEST_HASH replayed within 5 s" \
		"sample: $(head -30 "$SSE2")"
fi

# --- 3. First replayed id is exactly Last-Event-ID + 1. ----------
FIRST_REPLAY_ID=$(grep "^id: " "$SSE2" | head -1 | sed 's/^id: //')
EXPECTED_NEXT=$((LAST_ID + 1))
if [ -n "$FIRST_REPLAY_ID" ] && [ "$FIRST_REPLAY_ID" -eq "$EXPECTED_NEXT" ] 2>/dev/null; then
	_pass "First replayed event id is Last-Event-ID+1 ($EXPECTED_NEXT)"
else
	_fail "Replay starts at Last-Event-ID+1" \
		"expected id $EXPECTED_NEXT, got '$FIRST_REPLAY_ID'"
fi

# --- 4. Replayed ids are strictly monotonic and gap-free. --------
IDS=$(grep "^id: " "$SSE2" | sed 's/^id: //')
N_IDS=$(echo "$IDS" | wc -l | tr -d ' ')
prev=$LAST_ID
GAP_FOUND=0
NONMONO=0
while IFS= read -r id; do
	if [ "$id" -le "$prev" ] 2>/dev/null; then
		NONMONO=1
		break
	fi
	if [ "$id" -ne $((prev + 1)) ] 2>/dev/null; then
		GAP_FOUND=1
	fi
	prev=$id
done <<< "$IDS"
if [ "$NONMONO" -eq 0 ]; then
	_pass "Replayed event ids are strictly monotonic ($N_IDS events)"
else
	_fail "Replay monotonicity" "ids: $(echo "$IDS" | tr '\n' ' ')"
fi
if [ "$GAP_FOUND" -eq 0 ]; then
	_pass "Replayed event ids are gap-free (consecutive)"
else
	_fail "Replay gap-free" \
		"ids: $(echo "$IDS" | tr '\n' ' ')" \
		"a hole means an event was lost between Last-Event-ID and the resume point"
fi

# --- 5. Last-Event-ID > NewestId clamps to NewestId (no hang). ---
#
# Send an id from "another daemon process" — much higher than
# anything we've emitted. Daemon must clamp to NewestId() and
# behave like a fresh connect. We verify by checking the
# connection accepts the request and produces the heartbeat
# fallback (no replay = no events = `: keepalive` within timeout)
# rather than blocking forever.

CLAMP_ID=999999999999
: > "$SSE2"
(curl -s -m 4 -N \
	-H "Last-Event-ID: $CLAMP_ID" \
	-H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE2" 2>&1) &
PID3=$!
sleep 3.5
kill $PID3 2>/dev/null
wait $PID3 2>/dev/null
# We expect the `: connected` chunk (always emitted) and NO replay
# (since the requested id is in the future). Connection should not
# wedge — verify by checking the chunked stream produced *something*
# and that no events with id <= CLAMP_ID slipped through.
if grep -q "^: connected$" "$SSE2"; then
	_pass "Last-Event-ID > NewestId() does not hang the connection"
else
	_fail "Future Last-Event-ID handling" \
		"no ': connected' chunk; sample: $(head -10 "$SSE2")"
fi
# Any events that surface during this window must have id <= NewestId
# at this moment — they're new events the refresher emitted, NOT
# replay of the bogus future id.
FUTURE_REPLAY=$(grep "^id: " "$SSE2" | awk -v cap="$CLAMP_ID" '$0+0 >= cap')
if [ -z "$FUTURE_REPLAY" ]; then
	_pass "Future-id request does not replay phantom events"
else
	_fail "Future Last-Event-ID phantom replay" \
		"saw ids >= $CLAMP_ID: $FUTURE_REPLAY"
fi

# Below-OldestId gap detection is tested in 24-sse-resync.sh with the
# typed `resync` event (the wire shape 8c shipped as a `: replay-gap`
# SSE comment was upgraded to the typed event in 8d).

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
