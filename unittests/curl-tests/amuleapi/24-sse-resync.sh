#!/usr/bin/env bash
#
# amuleapi 24-sse-resync — typed `resync` event + log_appended.
#
# Wire contract for Phase 8d:
#   * `resync` event replaces Phase 8c's `: replay-gap` SSE
#     comment. It's a synthetic per-subscriber event (never on the
#     shared bus) with payload:
#       {"reason":"gap"|"restart","since_id":N,"newest_id":M}
#     - reason=gap     — Last-Event-ID < OldestId; events evicted
#                        before this subscriber could read them
#     - reason=restart — Last-Event-ID > NewestId; client's id is
#                        from a prior daemon process (per-process
#                        ids reset on restart)
#     id: <newest_id> so the client's EventSource resumes from the
#     current high water on the next reconnect.
#   * `log_appended` event: published from the refresher when
#     amuled's amule log grows. Payload:
#       {"lines":["...","..."]}
#     Batches all new lines from one tick into one event.
#     log_appended is best-effort to trigger in a smoke (amuled
#     logs sparsely during normal operation); the unit test
#     `EventDiffTest` covers the cold-start gate, batching, JSON
#     escaping, truncation, and idle ticks deterministically.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
BIN=${AMULEAPI_BIN:?orchestrator must export AMULEAPI_BIN}
CONFIG_DIR=${AMULEAPI_CONFIG_DIR:?orchestrator must export AMULEAPI_CONFIG_DIR}
LOG=${AMULEAPI_LOG:-/tmp/amuleapi.log}

FAIL_COUNT=0
TEST_COUNT=0

SSE=$(mktemp -t amuleapi_24-sse-resync.XXXXXX)
trap 'rm -f "$SSE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_skip() { TEST_COUNT=$((TEST_COUNT+1)); echo "  SKIP  $1"; }
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

echo "amuleapi 24-sse-resync smoke @ $HOST"

# Bounce the daemon with a tiny [Streaming]/EventBusRingCapacity so
# the gap case (Last-Event-ID < OldestId) is reachable in a smoke
# window. The production default is 16384, well past anything the
# refresher emits in a 20 s wait against an idle daemon. Other
# defaults are preserved from the orchestrator's first-run write.
pkill -f "amuleapi --config-dir=$CONFIG_DIR" 2>/dev/null
sleep 1
cat > "$CONFIG_DIR/amuleapi.conf" <<EOF
[Server]
BindAddress=127.0.0.1
Port=4713

[EC]
Host=127.0.0.1
Port=4712
Password=

[Auth]
LoginFailureWindowSeconds=60
LoginFailureThreshold=5
LoginLockoutSeconds=300

[Streaming]
EventBusRingCapacity=4
EOF
"$BIN" --config-dir="$CONFIG_DIR" \
	--host=127.0.0.1 --port=4712 --password=amule \
	> "$LOG" 2>&1 &
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	if curl -s -o /dev/null --max-time 1 "$HOST/api/v0/version" 2>/dev/null; then
		break
	fi
	sleep 0.5
done
# Re-login: the bus restart minted a new daemon process, so the
# token from before the bounce is unrelated (its rate-limit bucket
# is empty too).
ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed after bounce"

# Deterministically rotate the ring past id 1 instead of hoping the
# ambient refresher emits >capacity events in a fixed sleep (flaky on
# a quiet daemon). With the bus at 4 slots, a few add/delete cycles —
# each a download_added + download_removed the refresher observes on
# its next tick — guarantee OldestId climbs above 1. Each op is spaced
# past one tick so the refresher can't coalesce an add+delete pair
# into a no-op. End on a delete so the queue is clean for test 4.
FILL_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
FILL_HASH="0031c9cba65c50dd2015c184b2ca2c88"
for _ in 1 2 3 4; do
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"ed2k_link\":\"$FILL_LINK\"}" "$HOST/api/v0/downloads"
	sleep 2
	curl -s -o /dev/null -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads/$FILL_HASH"
	sleep 2
done
# Final settle so the last download_removed is in the ring.
sleep 1

# --- 1. resync(reason=gap) — Last-Event-ID below OldestId. -------
#
# After 8 s of refresher ticks the 32-slot ring has rotated past
# id 1, so `Last-Event-ID: 1` is in the gap range. Expect the
# synthetic `resync` event first.
: > "$SSE"
(curl -s -m 3 -N \
	-H "Last-Event-ID: 1" \
	-H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE" 2>&1) &
PID=$!
sleep 2.5
kill $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "^event: resync$" "$SSE"; then
	_pass "resync event fires when Last-Event-ID is below OldestId"
else
	_fail "resync(gap) event" \
		"no 'event: resync' line in stream; sample: $(head -20 "$SSE")"
fi

# Payload should be JSON with reason=gap, since_id=1, newest_id>0.
RESYNC_DATA=$(grep -A2 "^event: resync$" "$SSE" \
	| grep "^data: " | head -1 | sed 's/^data: //')
if [ -n "$RESYNC_DATA" ]; then
	REASON=$(echo "$RESYNC_DATA" | jq -r .reason 2>/dev/null)
	SINCE=$(echo "$RESYNC_DATA" | jq -r .since_id 2>/dev/null)
	NEWEST=$(echo "$RESYNC_DATA" | jq -r .newest_id 2>/dev/null)
	if [ "$REASON" = "gap" ]; then
		_pass "resync.reason == 'gap'"
	else
		_fail "resync.reason for gap case" "expected 'gap', got '$REASON' from $RESYNC_DATA"
	fi
	if [ "$SINCE" = "1" ]; then
		_pass "resync.since_id echoes the client-sent Last-Event-ID"
	else
		_fail "resync.since_id" "expected 1, got '$SINCE'"
	fi
	if [ -n "$NEWEST" ] && [ "$NEWEST" != "null" ] && [ "$NEWEST" -gt 0 ] 2>/dev/null; then
		_pass "resync.newest_id is a positive integer ($NEWEST)"
	else
		_fail "resync.newest_id" "expected positive int, got '$NEWEST'"
	fi
else
	_fail "resync data line" "no 'data:' line under resync event in $SSE"
fi

# Phase 8c's `: replay-gap` comment must NOT appear anymore —
# replaced by the typed event.
if grep -q "^: replay-gap$" "$SSE"; then
	_fail "Phase 8c comment is gone" \
		"': replay-gap' comment still appears alongside resync event — should be removed"
else
	_pass "Phase 8c ': replay-gap' comment is no longer emitted"
fi

# --- 2. resync(reason=restart) — Last-Event-ID above NewestId. ---
: > "$SSE"
(curl -s -m 3 -N \
	-H "Last-Event-ID: 999999999999" \
	-H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE" 2>&1) &
PID=$!
sleep 2.5
kill $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -q "^event: resync$" "$SSE"; then
	_pass "resync event fires when Last-Event-ID is above NewestId"
else
	_fail "resync(restart) event" \
		"no 'event: resync' line; sample: $(head -10 "$SSE")"
fi
RESYNC_DATA=$(grep -A2 "^event: resync$" "$SSE" \
	| grep "^data: " | head -1 | sed 's/^data: //')
REASON=$(echo "$RESYNC_DATA" | jq -r .reason 2>/dev/null)
if [ "$REASON" = "restart" ]; then
	_pass "resync.reason == 'restart' for above-NewestId case"
else
	_fail "resync.reason for restart case" \
		"expected 'restart', got '$REASON' from $RESYNC_DATA"
fi

# --- 3. resync.id == newest_id so the client's next reconnect ----
#     resumes from the high water (no resync loop).
RESYNC_ID=$(grep -B1 "^data: {.reason" "$SSE" | grep "^id: " | head -1 | sed 's/^id: //')
NEWEST_FROM_PAYLOAD=$(echo "$RESYNC_DATA" | jq -r .newest_id 2>/dev/null)
if [ -n "$RESYNC_ID" ] && [ "$RESYNC_ID" = "$NEWEST_FROM_PAYLOAD" ]; then
	_pass "resync event id == newest_id ($RESYNC_ID) — no resync loop on next reconnect"
else
	_fail "resync event id matches newest_id" \
		"frame id='$RESYNC_ID' payload newest_id='$NEWEST_FROM_PAYLOAD' — must match"
fi

# --- 4. log_appended — best-effort: trigger via POST /downloads. -
#
# amuled logs sparsely during normal operation, so this test is
# best-effort. If no log_appended fires within the window we SKIP
# rather than fail (EventDiffTest covers the wiring deterministically).
# When the event DOES fire, validate the payload shape.

# First clear any prior in-queue test artifact so the POST
# below actually does work (and potentially logs).
TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"
TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null 2>&1 || true
sleep 3

: > "$SSE"
(curl -s -m 14 -N \
	-H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE" 2>&1) &
PID=$!
sleep 2
curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" \
	"$HOST/api/v0/downloads" > /dev/null
sleep 11
kill $PID 2>/dev/null
wait $PID 2>/dev/null

LOG_EVENT_COUNT=$(grep -c "^event: log_appended$" "$SSE" || true)
if [ "$LOG_EVENT_COUNT" -ge 1 ]; then
	_pass "log_appended event observed ($LOG_EVENT_COUNT events) — wire path confirmed"
	# Validate payload shape: must have a .lines array of strings.
	LOG_DATA=$(grep -A2 "^event: log_appended$" "$SSE" \
		| grep "^data: " | head -1 | sed 's/^data: //')
	if echo "$LOG_DATA" | jq -e '.lines | type == "array"' >/dev/null 2>&1; then
		_pass "log_appended.data.lines is an array"
	else
		_fail "log_appended payload shape" \
			"expected {lines:[...]}, got: $LOG_DATA"
	fi
	if echo "$LOG_DATA" | jq -e '.lines | length > 0' >/dev/null 2>&1; then
		_pass "log_appended.data.lines is non-empty"
	else
		_fail "log_appended.data.lines length" \
			"empty lines array — refresher fired the event with no new lines"
	fi
else
	_skip "log_appended observable in smoke window (amuled logs are sparse; EventDiffTest covers the wiring)"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
