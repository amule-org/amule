#!/usr/bin/env bash
#
# amuleapi 26-rfc-followup-endpoints — endpoints added to align with the RFC PR #132
# review:
#
#   * GET    /status                       — `kad.network: {users,files,nodes}` rollup
#   * POST   /shared/reload                — rescan share roots
#   * POST   /servers/update               — refresh server list from server.met URL
#   * POST   /servers/<ip>:<port>/connect  — address-keyed alias
#   * DELETE /servers/<ip>:<port>          — address-keyed alias
#   * DELETE /logs/amule                   — clear amule log + in-process cache
#   * DELETE /logs/serverinfo              — clear MOTD log + invalidate lazy cache
#   * POST   /downloads {"links":[...]}    — array body, alongside `ed2k_link`
#   * POST   /networks/disconnect          — `{"network":"ed2k"|"kad"|"both"}` selector
#   * GET    /clients?filter=uploads|downloads|active
#   * GET    /events?channels=<csv>        — subscribe to a subset of event types
#
# All log-mutating tests verify that a *fast* GET immediately after the
# mutation returns post-mutation state (not stale cache).

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"
TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"

FAIL_COUNT=0
TEST_COUNT=0
SSE=$(mktemp -t amuleapi_26_rfc_followup_endpoints_sse.XXXXXX)
trap '
	rm -f "$SSE"
	# Best-effort: delete any partfile this script may have left in
	# amuled queue (Windows VM has ~63 GB on C: — a leftover 6.6 GB
	# Ubuntu ISO partfile blocks the next run). Errors swallowed
	# because the daemon may already be down on a CI tear-down.
	if [ -n "${ADMIN_TOKEN:-}" ]; then
		curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
			"$HOST/api/v0/downloads/$TEST_HASH" > /dev/null 2>&1 || true
	fi
' EXIT

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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version"; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 26-rfc-followup-endpoints smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"
H_AUTH=(-H "Authorization: Bearer $ADMIN_TOKEN")
sleep 4

# --- 1. /status kad.network rollup. ------------------------------
STATUS=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/status")
if echo "$STATUS" | jq -e '.kad.network | type == "object"' >/dev/null 2>&1; then
	_pass "/status .kad.network exists"
else
	_fail "/status kad.network" "missing: $(echo "$STATUS" | jq -c .kad)"
fi
for f in users files nodes; do
	if echo "$STATUS" | jq -e ".kad.network.$f | type == \"number\"" >/dev/null 2>&1; then
		_pass "/status .kad.network.$f is a number"
	else
		_fail "/status kad.network.$f" "missing/non-numeric"
	fi
done

# --- 2. POST /shared/reload. -------------------------------------
RC=$(curl -s -o /tmp/p11_shared_reload.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$HOST/api/v0/shared/reload")
if [ "$RC" = "202" ]; then
	_pass "POST /shared/reload → 202"
else
	_fail "shared/reload status" "expected 202, got $RC: $(cat /tmp/p11_shared_reload.json)"
fi
if jq -e '.ok == true' /tmp/p11_shared_reload.json >/dev/null 2>&1; then
	_pass "POST /shared/reload .ok == true"
else
	_fail "shared/reload body" "$(cat /tmp/p11_shared_reload.json)"
fi

# --- 3. POST /servers/update — body validation. ------------------
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" -d '{}' "$HOST/api/v0/servers/update")
if [ "$RC" = "400" ]; then
	_pass "POST /servers/update missing url → 400"
else
	_fail "servers/update no-body" "expected 400, got $RC"
fi
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"servers_url":"ftp://nope"}' "$HOST/api/v0/servers/update")
if [ "$RC" = "400" ]; then
	_pass "POST /servers/update non-http url → 400"
else
	_fail "servers/update bad url" "expected 400, got $RC"
fi
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"servers_url":"http://upd.emule-security.org/server.met"}' \
	"$HOST/api/v0/servers/update")
if [ "$RC" = "202" ]; then
	_pass "POST /servers/update valid url → 202"
else
	_fail "servers/update happy path" "got $RC: $(cat /tmp/p11_su.json)"
fi

# --- 4. /servers/<ip>:<port>/connect + DELETE alias. -------------
# 404 path is always testable.
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$HOST/api/v0/servers/0.0.0.0:1/connect")
if [ "$RC" = "404" ]; then
	_pass "POST /servers/<bogus-ip:port>/connect → 404"
else
	_fail "address alias 404" "expected 404, got $RC"
fi
RC=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "${H_AUTH[@]}" \
	"$HOST/api/v0/servers/0.0.0.0:1")
if [ "$RC" = "404" ]; then
	_pass "DELETE /servers/<bogus-ip:port> → 404"
else
	_fail "address alias DELETE 404" "expected 404, got $RC"
fi
# Positive path: only test if there's actually a server in the cache.
# The address field is the canonical "<ip-or-hostname>:<port>" string
# the daemon reports, so we use it verbatim (no need to convert the
# numeric `ip` field — `address` is the operator-meaningful form).
ADDR=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/servers" \
	| jq -r '.servers[0].address // empty')
if [ -n "$ADDR" ] && [ "$ADDR" != "null" ]; then
	RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
		"$HOST/api/v0/servers/$ADDR/connect")
	if [ "$RC" = "202" ] || [ "$RC" = "200" ]; then
		_pass "POST /servers/$ADDR/connect resolves alias and accepts ($RC)"
	else
		_fail "address alias connect" "expected 200/202, got $RC"
	fi
else
	_skip "address-keyed server connect (no servers in cache)"
fi

# --- 5. DELETE /logs/amule + freshness. --------------------------
RC=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "${H_AUTH[@]}" \
	"$HOST/api/v0/logs/amule")
if [ "$RC" = "204" ]; then
	_pass "DELETE /logs/amule → 204"
else
	_fail "logs/amule DELETE" "expected 204, got $RC"
fi
# Fast GET immediately after — must show empty / post-reset state.
GET_BODY=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/logs/amule")
LINES=$(echo "$GET_BODY" | jq -r '.lines | length' 2>/dev/null)
TOTAL=$(echo "$GET_BODY" | jq -r '.total_cached' 2>/dev/null)
if [ "$LINES" = "0" ] && [ "$TOTAL" = "0" ]; then
	_pass "GET /logs/amule immediately after DELETE returns empty (no stale cache)"
else
	_fail "logs/amule post-DELETE freshness" \
		"lines=$LINES total=$TOTAL — expected 0/0"
fi

# --- 6. DELETE /logs/serverinfo + freshness (lazy cache!). -------
RC=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "${H_AUTH[@]}" \
	"$HOST/api/v0/logs/serverinfo")
if [ "$RC" = "204" ]; then
	_pass "DELETE /logs/serverinfo → 204"
else
	_fail "logs/serverinfo DELETE" "expected 204, got $RC"
fi
# Fast GET immediately after — must show empty / post-reset.
GET_BODY=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/logs/serverinfo")
BYTES=$(echo "$GET_BODY" | jq -r '.total_bytes' 2>/dev/null)
if [ "$BYTES" = "0" ]; then
	_pass "GET /logs/serverinfo immediately after DELETE returns empty (lazy cache invalidated)"
else
	_fail "logs/serverinfo post-DELETE freshness" \
		"total_bytes=$BYTES — expected 0"
fi

# --- 7. POST /downloads array body shape. ------------------------
# Cleanup any prior queue entry to make the test deterministic.
# A robust cleanup tries the DELETE, then waits until the file is
# really gone from /downloads (amuled processes DELETE asynchronously
# — the entry stays around for one or two refresher ticks while the
# partfile is unmapped). Without this, the next POST returns
# "Invalid link or already on list" and the test misreads the
# wire shape verdict.
_wait_for_no_download() {
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
		local present=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/downloads" \
			| jq -r --arg h "$TEST_HASH" \
				'.downloads | map(select(.hash == $h)) | length')
		if [ "$present" = "0" ]; then return 0; fi
		sleep 1
	done
	return 1
}
curl -s -X DELETE "${H_AUTH[@]}" "$HOST/api/v0/downloads/$TEST_HASH" > /dev/null
_wait_for_no_download || true

# Array form
RC=$(curl -s -o /tmp/p11_dl.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d "{\"links\":[\"$TEST_LINK\"]}" "$HOST/api/v0/downloads")
if [ "$RC" = "202" ]; then
	_pass "POST /downloads array form → 202"
else
	_fail "downloads array status" "expected 202, got $RC: $(cat /tmp/p11_dl.json)"
fi
# Unified per-item envelope (#358): one accepted result, no legacy counters.
if jq -e '(.results | length) == 1 and .results[0].ok == true' /tmp/p11_dl.json >/dev/null 2>&1; then
	_pass "POST /downloads array reports one ok result"
else
	_fail "downloads array results" "$(cat /tmp/p11_dl.json)"
fi
# Mixing both forms → 400
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\",\"links\":[\"$TEST_LINK\"]}" \
	"$HOST/api/v0/downloads")
if [ "$RC" = "400" ]; then
	_pass "POST /downloads mixing ed2k_link AND links → 400"
else
	_fail "downloads mixed body" "expected 400, got $RC"
fi
# Backwards-compat singular form still works.
curl -s -X DELETE "${H_AUTH[@]}" "$HOST/api/v0/downloads/$TEST_HASH" > /dev/null
_wait_for_no_download || true
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads")
if [ "$RC" = "202" ]; then
	_pass "POST /downloads singular ed2k_link still accepted (backwards-compat)"
else
	_fail "downloads singular body" "expected 202, got $RC"
fi

# --- 8. POST /networks/disconnect selector. ----------------------
# Default (no body) = both
RC=$(curl -s -o /tmp/p11_nd.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$HOST/api/v0/networks/disconnect")
if [ "$RC" = "200" ]; then
	_pass "POST /networks/disconnect (no body) → 200 default=both"
else
	_fail "networks disconnect no-body" "expected 200, got $RC"
fi
sleep 2
# selector=ed2k
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"network":"ed2k"}' "$HOST/api/v0/networks/disconnect")
if [ "$RC" = "200" ]; then
	_pass "POST /networks/disconnect {\"network\":\"ed2k\"} → 200"
else
	_fail "networks disconnect ed2k" "expected 200, got $RC"
fi
sleep 1
# selector=kad
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"network":"kad"}' "$HOST/api/v0/networks/disconnect")
if [ "$RC" = "200" ]; then
	_pass "POST /networks/disconnect {\"network\":\"kad\"} → 200"
else
	_fail "networks disconnect kad" "expected 200, got $RC"
fi
# Invalid selector
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"network":"icq"}' "$HOST/api/v0/networks/disconnect")
if [ "$RC" = "400" ]; then
	_pass "POST /networks/disconnect bogus selector → 400"
else
	_fail "networks disconnect bogus" "expected 400, got $RC"
fi

# --- 9. /clients filter. -----------------------------------------
# Get baseline first
TOTAL_CLIENTS=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients" \
	| jq '.clients | length')
UP_CLIENTS=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients?filter=uploads" \
	| jq '.clients | length')
DOWN_CLIENTS=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients?filter=downloads" \
	| jq '.clients | length')
ACTIVE_CLIENTS=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients?filter=active" \
	| jq '.clients | length')
if [ "$UP_CLIENTS" -le "$TOTAL_CLIENTS" ] 2>/dev/null \
   && [ "$DOWN_CLIENTS" -le "$TOTAL_CLIENTS" ] 2>/dev/null \
   && [ "$ACTIVE_CLIENTS" -le "$TOTAL_CLIENTS" ] 2>/dev/null; then
	_pass "/clients filters: total=$TOTAL_CLIENTS up=$UP_CLIENTS down=$DOWN_CLIENTS active=$ACTIVE_CLIENTS (each ≤ total)"
else
	_fail "clients filter sizing" \
		"total=$TOTAL_CLIENTS up=$UP_CLIENTS down=$DOWN_CLIENTS active=$ACTIVE_CLIENTS"
fi
# `active` = |uploads ∪ downloads|, so it sits in
# [max(up, down), up+down]. The lower bound holds because every
# uploads-or-downloads-only peer is in active; the upper bound holds
# because a peer simultaneously in both states (upload_state=uploading
# AND download_state=downloading) gets counted once in active but
# twice in (up + down). Exact equality is intentionally not asserted
# because the intersection count is unobservable from filter results
# alone.
EXP_UPPER=$((UP_CLIENTS + DOWN_CLIENTS))
if [ "$ACTIVE_CLIENTS" -ge "$UP_CLIENTS" ] 2>/dev/null \
   && [ "$ACTIVE_CLIENTS" -ge "$DOWN_CLIENTS" ] 2>/dev/null \
   && [ "$ACTIVE_CLIENTS" -le "$EXP_UPPER" ] 2>/dev/null; then
	_pass "/clients?filter=active sits in [max(up,down), up+down]"
else
	_fail "clients active span" \
		"active=$ACTIVE_CLIENTS up=$UP_CLIENTS down=$DOWN_CLIENTS (expected max..sum)"
fi
# Verify every entry in /clients?filter=uploads truly has upload_state=uploading
BAD=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients?filter=uploads" \
	| jq -r '.clients[] | select(.upload_state != "uploading") | .client_ecid' \
	| head -1)
if [ -z "$BAD" ]; then
	_pass "/clients?filter=uploads only returns upload_state=uploading peers"
else
	_fail "clients uploads filter content" \
		"client_ecid $BAD has wrong upload_state"
fi
# Bogus filter → 400
RC=$(curl -s -o /dev/null -w "%{http_code}" "${H_AUTH[@]}" \
	"$HOST/api/v0/clients?filter=alphabetical")
if [ "$RC" = "400" ]; then
	_pass "/clients?filter=<bogus> → 400"
else
	_fail "clients bogus filter" "expected 400, got $RC"
fi

# --- 9b. /clients/{ecid} detail (issue #422). --------------------
# A superset of the list object: every list field plus the detail-only
# B fields. Guarded on a live peer; the negative cases run regardless.
FIRST_ECID=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients" \
	| jq -r '.clients[0].client_ecid // empty')
if [ -n "$FIRST_ECID" ]; then
	DETAIL=$(curl -s "${H_AUTH[@]}" "$HOST/api/v0/clients/$FIRST_ECID")
	OK=$(echo "$DETAIL" | jq -r --argjson e "$FIRST_ECID" '
		(.client_ecid == $e)
		and has("user_hash")
		and has("user_id_hybrid") and has("high_id")
		and has("server_ip") and has("server_port") and has("server_name")
		and has("kad_port") and has("source_origin")
		and has("available_parts") and has("mod_version")
		and has("view_shared_disabled")
		and has("is_friend") and has("dl_up_modifier")')
	if [ "$OK" = "true" ]; then
		_pass "/clients/{ecid} returns detail superset (ecid=$FIRST_ECID)"
	else
		_fail "clients detail shape" "missing detail fields: $DETAIL"
	fi
else
	_pass "/clients/{ecid} detail 200 skipped (no peers connected)"
fi
# Unknown ecid → 404 (0xFFFFFFFF is effectively never a live ECID)
RC=$(curl -s -o /dev/null -w "%{http_code}" "${H_AUTH[@]}" "$HOST/api/v0/clients/4294967295")
if [ "$RC" = "404" ]; then
	_pass "/clients/{unknown-ecid} → 404"
else
	_fail "clients detail 404" "expected 404, got $RC"
fi
# Non-numeric ecid path → 400
RC=$(curl -s -o /dev/null -w "%{http_code}" "${H_AUTH[@]}" "$HOST/api/v0/clients/not-a-number")
if [ "$RC" = "400" ]; then
	_pass "/clients/{non-numeric} → 400"
else
	_fail "clients detail 400" "expected 400, got $RC"
fi
# Non-GET → 405
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" "$HOST/api/v0/clients/1")
if [ "$RC" = "405" ]; then
	_pass "POST /clients/{ecid} → 405"
else
	_fail "clients detail 405" "expected 405, got $RC"
fi

# --- 10. /events ?channels= filter. ------------------------------
# Test 8 left the daemon disconnected; reconnect so download_* /
# status_* events have a reason to fire.
curl -s -X POST "${H_AUTH[@]}" "$HOST/api/v0/networks/connect" > /dev/null
sleep 6
curl -s -X DELETE "${H_AUTH[@]}" "$HOST/api/v0/downloads/$TEST_HASH" > /dev/null
_wait_for_no_download || true
: > "$SSE"
( curl -s -m 10 -N "${H_AUTH[@]}" \
	"$HOST/api/v0/events?channels=downloads,status" \
	>> "$SSE" 2>&1 ) &
PID=$!
sleep 2
curl -s -X POST "${H_AUTH[@]}" -H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads" > /dev/null
sleep 6
kill $PID 2>/dev/null
wait $PID 2>/dev/null

# In the channels=downloads,status window we MUST see download_* / status_*
# events and MUST NOT see client_* / server_* / shared_* / log_* events.
if grep -qE "^event: (download_added|download_updated|status_changed)$" "$SSE"; then
	_pass "/events?channels=downloads,status delivers download/status events"
else
	_fail "events channel-filter positive" \
		"no download/status events seen; sample: $(head -10 "$SSE")"
fi
LEAKED=$(grep -cE "^event: (client_|server_|shared_|log_|search_)" "$SSE" || true)
if [ "$LEAKED" -eq 0 ]; then
	_pass "/events?channels=downloads,status excludes client/server/shared/log/search events"
else
	_fail "events channel-filter leak" \
		"$LEAKED off-channel events leaked through"
fi

# Positive: ?channels=search delivers search events and excludes downloads.
: > "$SSE"
( curl -s -m 12 -N "${H_AUTH[@]}" \
	"$HOST/api/v0/events?channels=search" \
	>> "$SSE" 2>&1 ) &
PID=$!
sleep 1
curl -s -X POST "${H_AUTH[@]}" -H "Content-Type: application/json" \
	-d '{"query":"ubuntu","type":"local"}' "$HOST/api/v0/search" > /dev/null
sleep 8
kill $PID 2>/dev/null
wait $PID 2>/dev/null

if grep -qE "^event: search_progress$" "$SSE"; then
	_pass "/events?channels=search delivers search_progress"
else
	_fail "events channel=search positive" \
		"no search_progress in 8 s; sample: $(head -10 "$SSE")"
fi
LEAKED=$(grep -cE "^event: (download_|status_|client_|server_|shared_|log_)" "$SSE" || true)
if [ "$LEAKED" -eq 0 ]; then
	_pass "/events?channels=search excludes non-search events"
else
	_fail "events channel=search leak" \
		"$LEAKED off-channel events leaked through"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
