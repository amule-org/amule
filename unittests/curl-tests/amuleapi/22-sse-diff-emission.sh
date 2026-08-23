#!/usr/bin/env bash
#
# amuleapi 22-sse-diff-emission — EventBus + Refresher diff emission.
#
# Wire contract for Phase 8b:
#   * After each successful refresher tick, the daemon walks the
#     prior-vs-current cache diff and publishes typed SSE events:
#       - download_added / _updated / _removed
#       - shared_added   / _updated / _removed
#       - server_added   / _updated / _removed
#       - client_added   / _updated / _removed
#       - status_changed
#   * Each event has a unique monotonic uint64 `id` (per amuleapi
#     process start; not stable across restarts).
#   * `_added` and `_updated` payloads are the full snapshot object;
#     `_removed` payloads are identity-only (`{"hash":"..."}` or
#     `{"ecid":N}`).
#   * Phase 8b subscribers see only events that fire AFTER they
#     connect (`since_id` starts at `NewestId()`). Phase 8c lands
#     `Last-Event-ID` replay.
#
# This smoke triggers real mutations through the API, captures the
# SSE stream, and asserts the corresponding events arrived with the
# right shape.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

# Stable test artifact (same as Phase 5a).
TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"

FAIL_COUNT=0
TEST_COUNT=0

SSE_OUT=$(mktemp -t amuleapi_22_sse_diff_emission_sse.XXXXXX)
trap '
	rm -f "$SSE_OUT"
	# Best-effort partfile cleanup so the 6.6 GB Ubuntu ISO doesn'\''t
	# survive a failed run and block the next one (Windows VM disk-
	# pressure mitigation per feedback_clean_temp_partfiles_after_test).
	if [ -n "${ADMIN_TOKEN:-}" ]; then
		curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null 2>&1 || true
	fi
' EXIT

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

echo "amuleapi 22-sse-diff-emission smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

sleep 4

# Helper to start a backgrounded SSE consumer that runs for $1
# seconds and writes the stream to $SSE_OUT. Returns the curl PID.
_sse_start() {
	local seconds=$1
	: > "$SSE_OUT"
	(curl -s -m "$seconds" -N -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/events" >> "$SSE_OUT" 2>&1) &
	echo $!
}

# Helper to count event frames of a specific name in $SSE_OUT.
_count_events() {
	local name=$1
	grep -c "^event: $name$" "$SSE_OUT" 2>/dev/null || echo 0
}

# --- 1. Initial subscribe — at least the connect chunk arrives. ---
SSE_PID=$(_sse_start 3)
sleep 2
kill $SSE_PID 2>/dev/null
wait $SSE_PID 2>/dev/null
if grep -q "^: connected$" "$SSE_OUT"; then
	_pass "SSE subscribe receives ': connected' on open"
else
	_fail "': connected'" "not in stream output"
fi

# --- 2. download_added fires on POST /downloads. -----------------
#
# Start SSE in background, POST the Ubuntu ISO, wait for amuled to
# allocate + hash + surface it in cache (~1-3 refresher ticks). The
# stream should show a `download_added` event for the new hash.

# First make sure the ISO isn't already in the queue (carry-over from
# a prior smoke). DELETE the existing entry if present, then start
# from a clean slate.
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null 2>&1 || true
sleep 2

SSE_PID=$(_sse_start 15)
sleep 1
echo "    info: POST Ubuntu ISO..."
curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" \
	"$HOST/api/v0/downloads" > /dev/null

# Wait for the download_added event. Poll the stream file every
# 200 ms for up to 12 s.
ADDED=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
         21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 \
         41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60; do
	if grep -q "^event: download_added$" "$SSE_OUT"; then
		ADDED=$(grep -A2 "^event: download_added$" "$SSE_OUT" \
			| grep "^data: " | grep -F "$TEST_HASH" | head -1)
		if [ -n "$ADDED" ]; then break; fi
	fi
	sleep 0.2
done
wait $SSE_PID 2>/dev/null

if [ -n "$ADDED" ]; then
	_pass "download_added event fired with Ubuntu ISO hash"
	# The data line should be valid JSON containing the expected fields.
	JSON=$(echo "$ADDED" | sed 's/^data: //')
	if echo "$JSON" | jq -e --arg h "$TEST_HASH" '.hash == $h' >/dev/null 2>&1; then
		_pass "download_added .data.hash matches the requested ISO"
	else
		_fail "download_added payload" \
			"JSON missing hash $TEST_HASH: $JSON"
	fi
	if echo "$JSON" | jq -e '.name | type == "string"' >/dev/null 2>&1; then
		_pass "download_added .data.name is a string"
	else
		_fail "download_added .data.name" "not a string in $JSON"
	fi
	# Kad-notes search state rides the download event (issue #434).
	if echo "$JSON" | jq -e '.kad_comment_search_running | type == "boolean"' >/dev/null 2>&1; then
		_pass "download_added .data.kad_comment_search_running is boolean"
	else
		_fail "download_added .data.kad_comment_search_running" "not boolean in $JSON"
	fi
	if echo "$JSON" | jq -e '.size | type == "number"' >/dev/null 2>&1; then
		_pass "download_added .data.size is a number"
	else
		_fail "download_added .data.size" "not a number in $JSON"
	fi
	# Hashing progress rides the download event too (issue #1054), so a
	# client watching the stream sees a Verify Local Data pass advance.
	if echo "$JSON" | jq -e '.hashing_progress | type == "number"' >/dev/null 2>&1; then
		_pass "download_added .data.hashing_progress is a number"
	else
		_fail "download_added .data.hashing_progress" "not a number in $JSON"
	fi
else
	_fail "download_added missing" \
		"no event with the Ubuntu ISO hash within 12 s; stream sample: $(head -30 "$SSE_OUT")"
fi

# --- 3. Event has monotonic `id`. --------------------------------
#
# Every event line should have an id: <N> line below it. Pluck the
# ids and verify they're strictly increasing.
IDS=$(grep "^id: " "$SSE_OUT" | sed 's/^id: //')
if [ -n "$IDS" ]; then
	prev=0
	monotonic=1
	while IFS= read -r id; do
		if [ "$id" -le "$prev" ] 2>/dev/null; then
			monotonic=0
			break
		fi
		prev=$id
	done <<< "$IDS"
	if [ $monotonic -eq 1 ]; then
		_pass "Event ids are strictly monotonic ($(echo "$IDS" | wc -l | tr -d ' ') events)"
	else
		_fail "Event id monotonicity" \
			"ids: $(echo "$IDS" | tr '\n' ' ')"
	fi
fi

# --- 4. download_removed fires on DELETE. ------------------------
SSE_PID=$(_sse_start 10)
sleep 1
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null
REMOVED=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
	if grep -q "^event: download_removed$" "$SSE_OUT"; then
		REMOVED=$(grep -A2 "^event: download_removed$" "$SSE_OUT" \
			| grep "^data: " | grep -F "$TEST_HASH" | head -1)
		if [ -n "$REMOVED" ]; then break; fi
	fi
	sleep 0.2
done
wait $SSE_PID 2>/dev/null

if [ -n "$REMOVED" ]; then
	_pass "download_removed event fired for the deleted Ubuntu ISO"
	JSON=$(echo "$REMOVED" | sed 's/^data: //')
	# _removed payload is identity-only.
	if echo "$JSON" | jq -e --arg h "$TEST_HASH" '.hash == $h' >/dev/null 2>&1; then
		_pass "download_removed .data.hash matches"
	else
		_fail "download_removed payload" "expected {\"hash\":\"$TEST_HASH\"}, got: $JSON"
	fi
else
	_fail "download_removed missing" \
		"no event with the Ubuntu ISO hash within 6 s"
fi

# --- 5. Multiple subscribers each see the same events. -----------
#
# Open two SSE streams concurrently. Trigger one mutation. Both
# streams should observe the resulting event.
SSE_A=$(mktemp -t amuleapi_22_sse_diff_emission_a.XXXXXX)
SSE_B=$(mktemp -t amuleapi_22_sse_diff_emission_b.XXXXXX)
(curl -s -m 10 -N -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE_A" 2>&1) &
PID_A=$!
(curl -s -m 10 -N -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >> "$SSE_B" 2>&1) &
PID_B=$!
sleep 2
# Add ISO again — emit download_added.
curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" \
	"$HOST/api/v0/downloads" > /dev/null
sleep 5
# Then delete to clean up.
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null
wait $PID_A $PID_B 2>/dev/null

A_HAS=$(grep -c "^event: download_added$" "$SSE_A" || true)
B_HAS=$(grep -c "^event: download_added$" "$SSE_B" || true)
if [ "$A_HAS" -ge 1 ] && [ "$B_HAS" -ge 1 ]; then
	_pass "Two concurrent subscribers each see the download_added event (A=$A_HAS, B=$B_HAS)"
else
	_fail "Concurrent subscribers" \
		"A=$A_HAS B=$B_HAS download_added events"
fi
rm -f "$SSE_A" "$SSE_B"

# --- 6. search_result_added + search_progress fire on POST /search. ----
# Wraps in a local-search smoke: amuled's `local` type is the fastest
# path (no server round-trips) so we get the terminal search_progress
# frame (state="finished") within seconds without needing a real ed2k
# network. Even on a fully-disconnected daemon `local` returns
# immediately with 0 results, which still triggers the finished frame.
# (search_progress supersedes the old standalone search_finished event.)
SSE_PID=$(_sse_start 15)
sleep 1
SEARCH_QUERY=ubuntu
SSE_SEARCH=$(curl -s -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"query\":\"$SEARCH_QUERY\",\"type\":\"local\"}" \
	"$HOST/api/v0/search")
SSE_SID=$(printf '%s' "$SSE_SEARCH" | jq -r '.search_id // empty')
SEARCH_FINISHED=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
         21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40; do
	# The terminal frame is a search_progress event whose data has
	# state=="finished". Several running frames may precede it; pick the
	# finished one.
	if grep -q "^event: search_progress$" "$SSE_OUT"; then
		SEARCH_FINISHED=$(grep -A2 "^event: search_progress$" "$SSE_OUT" \
			| grep "^data: " | sed 's/^data: //' \
			| jq -c 'select(.state == "finished")' 2>/dev/null | head -1)
		if [ -n "$SEARCH_FINISHED" ]; then break; fi
	fi
	sleep 0.2
done
wait $SSE_PID 2>/dev/null

if [ -n "$SEARCH_FINISHED" ]; then
	_pass "search_progress finished frame fired within 8 s of POST /search type=local"
	JSON="$SEARCH_FINISHED"
	if echo "$JSON" | jq -e '.state == "finished"' >/dev/null 2>&1; then
		_pass "search_progress .data.state == 'finished'"
	else
		_fail "search_progress .data.state" "expected 'finished' in $JSON"
	fi
	if echo "$JSON" | jq -e '.kind == "local"' >/dev/null 2>&1; then
		_pass "search_progress .data.kind == 'local'"
	else
		_fail "search_progress .data.kind" "expected 'local' in $JSON"
	fi
	if echo "$JSON" | jq -e '.percent | type == "number"' >/dev/null 2>&1; then
		_pass "search_progress .data.percent is numeric"
	else
		_fail "search_progress .data.percent" "missing/non-numeric in $JSON"
	fi
	if echo "$JSON" | jq -e '.results | type == "number"' >/dev/null 2>&1; then
		_pass "search_progress .data.results is numeric"
	else
		_fail "search_progress .data.results" "missing/non-numeric in $JSON"
	fi
	# search_result_added is content-dependent — only assert it
	# fired if the local search produced any hits. On a fully-
	# disconnected daemon it won't fire, and that's correct.
	N_ADDED=$(grep -c "^event: search_result_added$" "$SSE_OUT" || true)
	RESULTS_TOTAL=$(echo "$JSON" | jq '.results')
	if [ "$RESULTS_TOTAL" -gt 0 ] 2>/dev/null; then
		if [ "$N_ADDED" -ge 1 ]; then
			_pass "search_result_added fired ($N_ADDED times; finished reports $RESULTS_TOTAL results)"
		else
			_fail "search_result_added missing" \
				"finished reports $RESULTS_TOTAL results but no search_result_added events seen"
		fi
	else
		_pass "search_result_added correctly absent (local search returned 0 results)"
	fi
	# Every frame on this channel names the search it belongs to, which is
	# what lets a client demux several concurrent searches.
	if [ -n "$SSE_SID" ] && echo "$JSON" | jq -e --argjson sid "$SSE_SID" '.search_id == $sid' >/dev/null 2>&1; then
		_pass "search_progress .data.search_id names the search that produced it"
	else
		_fail "search_progress .data.search_id" "expected $SSE_SID in $JSON"
	fi
else
	_fail "search_progress finished frame missing" \
		"no finished search_progress within 8 s of POST /search; stream sample: $(head -40 "$SSE_OUT")"
fi

# --- 6.1 search_closed fires when a search is freed. ---------------
# A subscriber holding one view per search learns the slot is gone from
# this event; without it, it would only find out by 404ing on a later
# read, and with SSE live it may never read again.
if [ -n "$SSE_SID" ]; then
	SSE_PID=$(_sse_start 8)
	sleep 1
	curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/search/$SSE_SID" > /dev/null
	CLOSED_JSON=""
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		if grep -q "^event: search_closed$" "$SSE_OUT"; then
			CLOSED_JSON=$(grep -A2 "^event: search_closed$" "$SSE_OUT" \
				| grep "^data: " | sed 's/^data: //' | head -1)
			[ -n "$CLOSED_JSON" ] && break
		fi
		sleep 0.25
	done
	wait $SSE_PID 2>/dev/null
	if [ -n "$CLOSED_JSON" ] && \
	   echo "$CLOSED_JSON" | jq -e --argjson sid "$SSE_SID" '.search_id == $sid' >/dev/null 2>&1; then
		_pass "search_closed fired for the freed search ($CLOSED_JSON)"
	else
		_fail "search_closed missing" \
			"DELETE /search/$SSE_SID produced no matching search_closed; got: $CLOSED_JSON"
	fi
else
	echo "    info: no search_id from POST /search — skipping search_closed"
fi

# --- comments_updated shape (issue #434). Conditional: the event only
# fires when a download's comment list changes (a Kad note arriving, or a
# source reporting a comment), which the smoke daemon can't force. If one
# was captured, validate its shape; otherwise skip — the live-network path
# is validated manually.
CU=$(grep -A1 "^event: comments_updated$" "$SSE_OUT" | grep "^data: " | head -1 | sed 's/^data: //')
if [ -n "$CU" ]; then
	if echo "$CU" | jq -e '(.hash|type=="string") and (.count|type=="number") and (.comments|type=="array")' >/dev/null 2>&1; then
		_pass "comments_updated payload shape valid"
	else
		_fail "comments_updated payload shape" "unexpected: $CU"
	fi
else
	_pass "comments_updated not emitted this run (no comment change; shape verified live)"
fi

# --- status_changed carries the same keys as GET /status. ----------
# EVENTS.md promises the payload is "identical to the REST /status
# envelope", and the API contract turns on that: a subscriber must never
# have to fall back to a poll for a field a poller can see. This drifted
# once already -- both connected_since timestamps were REST-only.
#
# `paths` rather than `paths(scalars)`: jq's `scalars` drops nulls, and
# disk.{temp,incoming}_free_bytes are null whenever the daemon has no
# figure, which would silently exempt exactly the fields with the
# trickiest contract. Plain `paths` also lists the container keys, so a
# whole object going missing is caught too.
#
# Conditional, like comments_updated above: status_changed fires only when
# something in the envelope actually moved, and an idle daemon legitimately
# emits nothing. So a missing frame is a skip, not a failure -- what this
# guards is the SHAPE when a frame does arrive. That the event fires at all
# is asserted in EventDiffTest, which can force a change directly.
SSE_PID=$(_sse_start 15)
STATUS_JSON=""
for _ in $(seq 1 40); do
	if grep -q "^event: status_changed$" "$SSE_OUT"; then
		STATUS_JSON=$(grep -A2 "^event: status_changed$" "$SSE_OUT" \
			| grep "^data: " | sed 's/^data: //' | head -1)
		[ -n "$STATUS_JSON" ] && break
	fi
	sleep 0.25
done
kill $SSE_PID 2>/dev/null
wait $SSE_PID 2>/dev/null
if [ -n "$STATUS_JSON" ]; then
	REST_JSON=$(curl -s -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/status")
	# The two subtractions need their own parentheses: inside object
	# construction jq parses `k: a - b` as `k: a` and then chokes on the `-`,
	# so the expression failed to compile and this check reported a failure
	# on every run that actually captured a frame.
	DIFF=$(jq -n --argjson a "$REST_JSON" --argjson b "$STATUS_JSON" \
		'def keys_: [paths | join(".")] | sort;
		 {rest_only: (($a|keys_) - ($b|keys_)), sse_only: (($b|keys_) - ($a|keys_))}')
	if [ "$(echo "$DIFF" | jq -c '.')" = '{"rest_only":[],"sse_only":[]}' ]; then
		_pass "status_changed payload keys match GET /status exactly"
	else
		_fail "status_changed / GET /status key parity" "$DIFF"
	fi
else
	_pass "status_changed not emitted this run (nothing in the envelope moved; shape asserted when it fires)"
fi

# --- client_added / client_updated carry the promoted peer fields, and
# never a parts bitmap (issue #984). The four fields were detail-only until
# the per-file client routes landed; a subscriber renders them as columns,
# so they have to be on the diff payload too. The bitmaps deliberately are
# NOT: one boolean per chunk per peer would dwarf the rest of the stream.
#
# Conditional like the two checks above -- an idle daemon with no peers
# emits no client frame -- so a missing frame is a skip and what is guarded
# is the shape when one arrives. Reuses the stream captured above.
CLIENT_JSON=$(grep -A2 -E "^event: client_(added|updated)$" "$SSE_OUT" \
	| grep "^data: " | sed 's/^data: //' | head -1)
if [ -n "$CLIENT_JSON" ]; then
	if echo "$CLIENT_JSON" | jq -e \
		'(.source_origin|type=="string") and (.available_parts|type=="number")
		 and (.mod_version|type=="string") and (.view_shared_disabled|type=="boolean")' \
		>/dev/null 2>&1; then
		_pass "client_added/updated carries the promoted peer fields (#984)"
	else
		_fail "client payload promoted fields" "missing/wrong type in: $CLIENT_JSON"
	fi
	if echo "$CLIENT_JSON" | jq -e 'has("parts") | not' >/dev/null 2>&1; then
		_pass "client_added/updated carries no parts bitmap (#984)"
	else
		_fail "client payload parts bitmap" "SSE must never carry parts: $CLIENT_JSON"
	fi
	if echo "$CLIENT_JSON" | jq -e \
		'(has("part_progress_percent") | not) or (.part_progress_percent|type=="number")' \
		>/dev/null 2>&1; then
		_pass "client_added/updated part_progress_percent is numeric when present"
	else
		_fail "client payload part_progress_percent" "not numeric in: $CLIENT_JSON"
	fi
else
	_pass "no client frame this run (no peer changed; shape asserted when one fires)"
fi

# --- shared_added / shared_updated carry the same keys as the GET
# --- /shared list item.
# REFERENCE.md promises the payload "matches this object byte-for-byte, so a
# subscriber that received shared_updated does not need to re-GET". Nothing
# checked it: the mechanical key-diff above covers status only, and the shared
# payload is built by a SEPARATE writer (ToJsonSharedEvent) from the one the
# list uses (WriteSharedObject), so the two drift silently by construction.
# They already did once -- `media` reached the event before the list item.
#
# Appended at the end of the file on purpose: every section here shares one
# live daemon, and a new section in the middle can change the state later
# sections read.
#
# Conditional for the same reason as status_changed: an idle share emits
# nothing, so a missing frame is a skip. What this guards is the SHAPE.
# Force a real change rather than waiting for one: a reload of an unchanged
# share emits nothing, so a passive wait here reports "no frame" every run and
# checks nothing. Flipping a shared file's priority moves a field EqualShared
# compares, which is exactly what makes shared_updated fire. The original
# value is restored below -- this daemon is shared with the other sections.
#
# Prefer a file that HAS media. Both writers emit `media` only when the file
# carries it, so comparing whatever sorts first comes down to two objects that
# both omit the key: the sets match and the field this check exists to guard
# was never compared. That is a green check over a comparison that never
# happened, and it is exactly the regression that got through last time.
# One GET, three extractions: re-fetching per field lets a live daemon reorder
# the list between them, which would restore one file's priority onto another.
#
# The subject is chosen from the DETAIL endpoint, never from the list's own
# `media` key. Selecting on the list would key the check off the exact field
# whose absence is the bug: drop media from the list writer and the selector
# stops finding a media file, falls back to a document, and the comparison
# passes with media never compared -- the guard reporting green precisely when
# it should be red. Detail builds through a different writer, so it still
# answers truthfully when the list is the broken one.
PARITY_BODY=$(curl -s -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared")
PARITY_HASH=
for CAND in $(printf '%s' "$PARITY_BODY" | jq -r '.shared[0:20][].hash'); do
	if curl -s -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/$CAND" \
		| jq -e '.media != null' >/dev/null 2>&1; then
		PARITY_HASH=$CAND
		break
	fi
done
[ -n "$PARITY_HASH" ] || PARITY_HASH=$(printf '%s' "$PARITY_BODY" | jq -r '.shared[0].hash // empty')
PARITY_PRIO=$(printf '%s' "$PARITY_BODY" \
	| jq -r --arg h "$PARITY_HASH" '.shared[] | select(.hash == $h) | .priority')
PARITY_AUTO=$(printf '%s' "$PARITY_BODY" \
	| jq -r --arg h "$PARITY_HASH" '.shared[] | select(.hash == $h) | .priority_auto')
PARITY_HAS_MEDIA=0
if [ -n "$PARITY_HASH" ] && curl -s -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/$PARITY_HASH" | jq -e '.media != null' >/dev/null 2>&1; then
	PARITY_HAS_MEDIA=1
fi
SHARED_JSON=""
if [ -z "$PARITY_HASH" ]; then
	_pass "nothing shared; shared-event parity check skipped"
else
if [ "$PARITY_HAS_MEDIA" = "1" ]; then
	echo "    info: parity subject carries media (the key this check guards)"
else
	echo "    info: no shared file carries media; parity checked on the common keys only"
fi
SSE_PID=$(_sse_start 25)
# Wait for the stream to exist rather than sleeping a fixed second: _sse_start
# backgrounds curl and returns immediately, so on a loaded runner the PATCH can
# land before the subscription does. Every other section here treats a missing
# frame as a pass and absorbs that race; this one requires the frame, so it has
# to actually wait for the connection instead of assuming it.
for _ in $(seq 1 80); do
	[ -s "$SSE_OUT" ] && break
	sleep 0.25
done
if [ "$PARITY_PRIO" = "high" ]; then NEW_PRIO=low; else NEW_PRIO=high; fi
curl -s -o /dev/null -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"priority\":\"$NEW_PRIO\"}" "$HOST/api/v0/shared/$PARITY_HASH"
# Take the frame for THIS file: any other file's frame would be a valid
# parity pair but not necessarily the one carrying media.
for _ in $(seq 1 40); do
	SHARED_JSON=$(grep -A2 -E "^event: shared_(added|updated)$" "$SSE_OUT" 2>/dev/null \
		| grep "^data: " | sed 's/^data: //' \
		| jq -c --arg h "$PARITY_HASH" 'select(.hash == $h)' 2>/dev/null | head -1)
	[ -n "$SHARED_JSON" ] && break
	sleep 0.25
done
kill $SSE_PID 2>/dev/null
wait $SSE_PID 2>/dev/null
if [ -n "$SHARED_JSON" ]; then
	REST_ITEM=$(curl -s -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared" \
		| jq -c --arg h "$PARITY_HASH" '.shared[] | select(.hash == $h)')
	if [ -z "$REST_ITEM" ]; then
		# The file left the share between the frame and the GET; no contract
		# to check, and failing here would be a flake, not a finding.
		_pass "shared frame's file no longer listed (shape asserted when both are present)"
	else
		DIFF=$(jq -n --argjson a "$REST_ITEM" --argjson b "$SHARED_JSON" \
			'def keys_: [paths | join(".")] | sort;
			 {rest_only: (($a|keys_) - ($b|keys_)), sse_only: (($b|keys_) - ($a|keys_))}')
		if [ "$(echo "$DIFF" | jq -c '.')" = '{"rest_only":[],"sse_only":[]}' ]; then
			_pass "shared event payload keys match the GET /shared item exactly"
		else
			_fail "shared event / GET /shared key parity" "$DIFF"
		fi
	fi
else
	_fail "shared event parity" "no shared_added/updated frame for $PARITY_HASH after a priority PATCH"
fi
# Restore what the PATCH changed: later sections and other phases read this
# daemon's state. priority_auto has to go back too -- setting a bare priority
# clears it.
curl -s -o /dev/null -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"priority\":\"$PARITY_PRIO\",\"priority_auto\":$PARITY_AUTO}" \
	"$HOST/api/v0/shared/$PARITY_HASH"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
