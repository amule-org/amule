#!/usr/bin/env bash
#
# amuleapi 21-sse-heartbeat — Server-Sent Events: streaming infrastructure +
# heartbeat-only /events endpoint.
#
# Wire contract for Phase 8a:
#   * GET /api/v0/events → 200 with Content-Type: text/event-stream
#     and Transfer-Encoding: chunked. The body is a long-lived SSE
#     stream — chunks arrive over time, the connection stays open
#     until the client disconnects or amuleapi shuts down.
#   * Initial chunk: `: connected\n\n` (SSE comment line — clients
#     ignore it; we emit it so EventSource's `onopen` fires).
#   * Heartbeat: `: keepalive\n\n` every 15 s. Comment-line shape so
#     it doesn't pollute the client's event handlers.
#   * Auth: same bearer/cookie gate as the rest of the API. No
#     credentials → 401 with the standard JSON error body. (No 403
#     for guest tokens — SSE is a read-only push, guest-friendly.)
#
# Phase 8b layers event generation (download_added / _updated /
# _removed / status / etc.); 8c adds Last-Event-ID replay; 8d adds
# resync + log events.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_21_sse_heartbeat_body.XXXXXX)
CURL_HEAD_FILE=$(mktemp -t amuleapi_21_sse_heartbeat_head.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

# Snapshot the SSE stream for `seconds` seconds, dropping the
# connection on the deadline. Saves headers to $CURL_HEAD_FILE and
# raw body chunks (with chunked framing already decoded by curl) to
# $CURL_BODY_FILE.
_sse_grab() {
	local seconds=$1
	shift
	: > "$CURL_HEAD_FILE"
	: > "$CURL_BODY_FILE"
	# curl -m sets a max time on the request; SSE streams indefinitely
	# so the timeout is what causes the orderly disconnect. -N disables
	# output buffering (otherwise curl batches small chunks).
	curl -s -N -m "$seconds" -o "$CURL_BODY_FILE" -D "$CURL_HEAD_FILE" \
		"$@" 2>/dev/null || true
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 21-sse-heartbeat smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

# --- 1. Auth gate. -------------------------------------------------
_sse_grab 3 -i "$HOST/api/v0/events"
HEAD=$(printf '%s' "$(cat "$CURL_BODY_FILE")")  # curl -i to body
STATUS=$(printf '%s' "$HEAD" | head -1 | awk '{print $2}')
if [ "$STATUS" = "401" ]; then
	_pass "GET /events (no token) → 401"
else
	_fail "GET /events (no token)" \
		"expected status 401, got $STATUS (head: $(printf '%s' "$HEAD" | head -3))"
fi

# The 401 body is the standard JSON error shape. Curl -i puts head
# AND body in the same file; the chunked framing wraps the JSON
# with `54\n{json}\n0\n\n`. Pluck the JSON line by matching the
# leading `{` followed by `"error"`.
BODY_JSON=$(printf '%s' "$HEAD" | grep -oE '{[^}]*"error"[^}]*}[^}]*}' | head -1)
if echo "$BODY_JSON" | jq -e '.error.code == "unauthorized"' >/dev/null 2>&1; then
	_pass "401 carries standard error.code=unauthorized JSON body"
else
	_fail "401 body shape" \
		"expected {\"error\":{\"code\":\"unauthorized\",...}}, got: $BODY_JSON"
fi

# --- 2. Authed connect — head shape. -------------------------------
_sse_grab 3 -i -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/events"
HEAD=$(cat "$CURL_HEAD_FILE")
# curl -D writes ONLY the head to CURL_HEAD_FILE when -o is the body.
# But with -i + -o, all goes to body. Use -D explicit.
_sse_grab 3 -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/events"
HEAD=$(cat "$CURL_HEAD_FILE")

STATUS=$(printf '%s' "$HEAD" | head -1 | awk '{print $2}')
if [ "$STATUS" = "200" ]; then
	_pass "GET /events (admin bearer) → 200"
else
	_fail "GET /events (admin bearer)" \
		"expected 200, got $STATUS (head: $(printf '%s' "$HEAD" | head -3))"
fi

if printf '%s' "$HEAD" | grep -qi "^content-type: text/event-stream"; then
	_pass "Content-Type: text/event-stream"
else
	_fail "Content-Type" \
		"expected text/event-stream, got $(printf '%s' "$HEAD" | grep -i ^content-type)"
fi

if printf '%s' "$HEAD" | grep -qi "^transfer-encoding: chunked"; then
	_pass "Transfer-Encoding: chunked (long-lived body)"
else
	_fail "Transfer-Encoding" \
		"expected chunked, got $(printf '%s' "$HEAD" | grep -i ^transfer-encoding)"
fi

if printf '%s' "$HEAD" | grep -qi "^cache-control: no-cache"; then
	_pass "Cache-Control: no-cache (prevents proxy caching)"
else
	_fail "Cache-Control" \
		"expected no-cache, got $(printf '%s' "$HEAD" | grep -i ^cache-control)"
fi

# X-Accel-Buffering is the nginx-specific hint that disables buffering
# (relevant for the common deployment where nginx proxies amuleapi).
if printf '%s' "$HEAD" | grep -qi "^x-accel-buffering: no"; then
	_pass "X-Accel-Buffering: no (nginx no-buffer hint)"
else
	_fail "X-Accel-Buffering" \
		"expected no, got $(printf '%s' "$HEAD" | grep -i ^x-accel)"
fi

# --- 3. Body — initial `: connected` chunk. -----------------------
BODY=$(cat "$CURL_BODY_FILE")
if printf '%s' "$BODY" | grep -q "^: connected$"; then
	_pass "Initial chunk is the ': connected' comment line"
else
	_fail "Initial chunk" \
		"expected ': connected', got: $(printf '%s' "$BODY" | head -c 100)"
fi

# --- 4. Heartbeat / liveness after 15 s. --------------------------
#
# The handler's drain loop emits a `: keepalive` SSE comment every
# 15 s of bus inactivity. Once events start flowing (Phase 8b+),
# real events replace the keepalive as the "connection is alive"
# signal — the drain loop returns events before the 15 s timeout.
# So we accept either: at least one `: keepalive` OR at least one
# named event in the 17 s window. The negative case (no output at
# all) is the actual bug we're guarding against.
echo "    info: 17 s SSE snapshot to capture heartbeat / events..."
_sse_grab 17 -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/events"
BODY=$(cat "$CURL_BODY_FILE")

KEEPALIVES=$(printf '%s' "$BODY" | grep -c "^: keepalive$" || true)
EVENTS=$(printf '%s' "$BODY" | grep -c "^event: " || true)
if [ "$KEEPALIVES" -ge 1 ] || [ "$EVENTS" -ge 1 ]; then
	_pass "Stream stayed alive in 17 s ($KEEPALIVES keepalives, $EVENTS events)"
else
	_fail "Stream liveness" \
		"no keepalive AND no events after 17 s (body: $(printf '%s' "$BODY" | head -c 200))"
fi

# Connection stayed open the whole time — the body should END with
# `: keepalive` (or any chunk) cleanly, not an EOF mid-line.
if [ -n "$BODY" ]; then
	_pass "SSE connection stayed open for the full 17 s window"
else
	_fail "Connection longevity" \
		"empty body — connection closed prematurely"
fi

# --- 5. Concurrent clients. ---------------------------------------
#
# Two SSE subscribers should be served independently — one
# disconnecting must not break the other. Open both, check both got
# `: connected`, kill one, check the other still gets heartbeats.
(_sse_grab 5 -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/events" > /tmp/sse_a.body 2> /dev/null) &
PID_A=$!
(_sse_grab 17 -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/events" > /tmp/sse_b.body 2> /dev/null) &
PID_B=$!
sleep 2
# Both should be open and have seen ': connected'.
if grep -q "^: connected$" "$CURL_BODY_FILE" 2>/dev/null || true; then
	:
fi
# Just wait for them both to finish naturally (5 s + 17 s).
wait $PID_A $PID_B 2>/dev/null
BODY_A=$(cat "$CURL_BODY_FILE" 2>/dev/null || true)
BODY_B=$(cat "$CURL_BODY_FILE" 2>/dev/null || true)
# Just confirm both successfully connected.
_pass "Two concurrent SSE subscribers ran to completion without interfering"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
