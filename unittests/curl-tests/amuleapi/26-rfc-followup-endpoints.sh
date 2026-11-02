#!/usr/bin/env bash
#
# amuleapi 26-rfc-followup-endpoints — endpoints added to align with the RFC PR #132
# review:
#
#   * GET    /status                       — `kad.network: {user_count,file_count,node_count}` rollup
#   * POST   /shared_reload                — rescan share roots
#   * POST   /servers_update               — refresh server list from server.met URL
#   * POST   /servers/by-address/<ip>:<port>/connect — address-keyed route
#   * DELETE /servers/by-address/<ip>:<port>         — address-keyed route
#   * DELETE /logs/amule                   — clear amule log + in-process cache
#   * DELETE /logs/server_info              — clear MOTD log + invalidate lazy cache
#   * POST   /downloads {"links":[...]}    — array body, the only spelling
#   * POST   /networks/disconnect          — `{"network":"ed2k"|"kad"|"both"}` selector
#   * GET    /clients?activity=uploading|downloading|active
#   * GET    /events?channels=<csv>        — subscribe to a subset of event types
#
# All log-mutating tests verify that a *fast* GET immediately after the
# mutation returns post-mutation state (not stale cache).

set -u
set -o pipefail

# The isolated search fixture enables Kad with an empty nodes.dat and never
# joins a public network. Reuse it so a real accepted search drives the SSE test.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" = "--offline" ]; then
	: "${AMULEAPI_BIN:?Run through run-all.sh or set AMULEAPI_BIN}"
	AMULED_BIN=${AMULED_BIN:-$(dirname "$AMULEAPI_BIN")/../amuled}
	exec python3 "$SCRIPT_DIR/../../tests/AllSearchApiSmoke.py" \
		"$AMULED_BIN" "$AMULEAPI_BIN" "$SCRIPT_DIR/26-rfc-followup-endpoints.sh"
fi

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

SSE_PROBE_HASH="0031c9cba65c50dd2015c184b2ca2c90"
SSE_PROBE_LINK="ed2k://|file|channel-filter-probe.bin|4096|0031C9CBA65C50DD2015C184B2CA2C90|/"
SSE_HASH="0031c9cba65c50dd2015c184b2ca2c89"
SSE_LINK="ed2k://|file|channel-filter-regression.bin|4096|0031C9CBA65C50DD2015C184B2CA2C89|/"

SSE=$(mktemp -t amuleapi_26_rfc_followup_endpoints_sse.XXXXXX)
PID=
cleanup() {
	local cleanup_hash
	if [ -n "$PID" ]; then
		kill "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
	rm -f "$SSE"
	# Best-effort: delete any partfile this script may have left in
	# amuled queue (Windows VM has ~63 GB on C: — a leftover 6.6 GB
	# Ubuntu ISO partfile blocks the next run). Errors swallowed
	# because the daemon may already be down on a CI tear-down.
	if [ -n "${ADMIN_TOKEN:-}" ]; then
		for cleanup_hash in "$TEST_HASH" "$SSE_PROBE_HASH" "$SSE_HASH"; do
			curl -s -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
				"$API/downloads/$cleanup_hash" > /dev/null 2>&1 || true
		done
	fi
}
_on_exit cleanup

_preflight

echo "amuleapi 26-rfc-followup-endpoints smoke @ $HOST"

ADMIN_TOKEN=$(_login "$ADMIN_PASS")
[ -n "$ADMIN_TOKEN" ] || _die "admin login failed"
H_AUTH=(-H "Authorization: Bearer $ADMIN_TOKEN")
sleep 4

# --- 1. /status kad.network rollup. ------------------------------
STATUS=$(curl -s "${H_AUTH[@]}" "$API/status")
if echo "$STATUS" | jq -e '.kad.network | type == "object"' >/dev/null 2>&1; then
	_pass "/status .kad.network exists"
else
	_fail "/status kad.network" "missing: $(echo "$STATUS" | jq -c .kad)"
fi
for f in user_count file_count node_count; do
	# Null is a measurement's explicit unknown value while disconnected.
	# Require the key as well: a missing field must not pass as a JSON null.
	if echo "$STATUS" | jq -e --arg field "$f" '
		.kad as $kad | ($kad.network | has($field)) and
		(if $kad.state == "connected" then
			$kad.network[$field] | type == "number"
		else $kad.network[$field] == null end)' >/dev/null 2>&1; then
		_pass "/status .kad.network.$f matches connection state"
	else
		_fail "/status kad.network.$f" "missing or inconsistent with Kad state: $STATUS"
	fi
done

# --- 2. POST /shared_reload. -------------------------------------
RC=$(curl -s -o /tmp/p11_shared_reload.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$API/shared_reload")
if [ "$RC" = "202" ]; then
	_pass "POST /shared_reload → 202"
else
	_fail "shared_reload status" "expected 202, got $RC: $(cat /tmp/p11_shared_reload.json)"
fi
# The 202 already said the call was accepted, so there is no constant `ok`.
# A body exists only when amuled had a message; with nothing to report the
# reply carries none, matching the URL-fetch triggers.
if [ ! -s /tmp/p11_shared_reload.json ]; then
	_pass "POST /shared_reload: 202 with no body (amuled reported nothing)"
elif jq -e '(has("ok") | not) and (.message | type == "string")' \
	/tmp/p11_shared_reload.json >/dev/null 2>&1; then
	_pass "POST /shared_reload: 202 body carries amuled's message and no constant ok"
else
	_fail "shared_reload body" "expected no body or {message}, got: $(cat /tmp/p11_shared_reload.json)"
fi

# --- 3. POST /servers_update — body validation. ------------------
# Use an empty saved list so this validation never starts an external download
# that could race the explicit-URL request below. Restore it afterwards.
SAVED_SOURCES=$(curl -s "${H_AUTH[@]}" "$API/preferences" | jq -c '{servers:{update_urls:.servers.update_urls}}')
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X PATCH "${H_AUTH[@]}" \
	-H "Content-Type: application/json" -d '{"servers":{"update_urls":[]}}' "$API/preferences")
if [ "$RC" != "200" ]; then
	_fail "servers_update setup" "expected 200, got $RC"
fi
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" -d '{}' "$API/servers_update")
if [ "$RC" = "400" ] && jq -e '.error.code == "amuled_rejected" and (.error.message | contains("No server list sources configured"))' /tmp/p11_su.json >/dev/null; then
	_pass "POST /servers_update empty sources → 400 with reason"
else
	_fail "servers_update no-body" "expected rejection for empty sources, got $RC"
fi
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"url":"ftp://nope"}' "$API/servers_update")
if [ "$RC" = "400" ]; then
	_pass "POST /servers_update non-http url → 400"
else
	_fail "servers_update bad url" "expected 400, got $RC"
fi
RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"url":"http://127.0.0.1:1/server.met"}' \
	"$API/servers_update")
if [ "$RC" = "202" ]; then
	_pass "POST /servers_update valid url → 202"
else
	_fail "servers_update happy path" "got $RC: $(cat /tmp/p11_su.json)"
fi

RC=$(curl -s -o /tmp/p11_su.json -w "%{http_code}" -X PATCH "${H_AUTH[@]}" \
	-H "Content-Type: application/json" -d "$SAVED_SOURCES" "$API/preferences")
if [ "$RC" != "200" ]; then
	_fail "servers_update restore sources" "expected 200, got $RC"
fi

# --- 4. /servers/by-address/<ip>:<port> connect + DELETE. --------
# 404 path is always testable. 192.0.2.x is TEST-NET-1 (RFC 5737),
# reserved for documentation and guaranteed never to be a real server,
# so this reaches the "well-formed but no such server" branch rather
# than being turned away earlier as bad input.
UNKNOWN_SRV=192.0.2.1:1
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$API/servers/by-address/$UNKNOWN_SRV/connect")
if [ "$RC" = "404" ]; then
	_pass "POST /servers/by-address/<unknown-ip:port>/connect → 404"
else
	_fail "address alias 404" "expected 404, got $RC"
fi
RC=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "${H_AUTH[@]}" \
	"$API/servers/by-address/$UNKNOWN_SRV")
if [ "$RC" = "404" ]; then
	_pass "DELETE /servers/by-address/<unknown-ip:port> → 404"
else
	_fail "address alias DELETE 404" "expected 404, got $RC"
fi
# 0.0.0.0 is well-formed but is not a server address, and it must never
# reach the lookup: a server row whose IP the daemon did not ship keeps
# ip == 0, so a 0.0.0.0 selector could otherwise resolve to whichever
# such row shared the port. Rejected as bad input, and the message says
# so rather than claiming a malformed quad.
BODY=$(curl -s -X POST "${H_AUTH[@]}" "$API/servers/by-address/0.0.0.0:4242/connect")
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$API/servers/by-address/0.0.0.0:4242/connect")
if [ "$RC" = "400" ] && printf '%s' "$BODY" | jq -e '.error.code == "bad_request"' >/dev/null 2>&1; then
	_pass "POST /servers/by-address/0.0.0.0:<port>/connect → 400 (not a server address)"
else
	_fail "0.0.0.0 selector rejected" "expected 400 bad_request, got $RC: $BODY"
fi
if printf '%s' "$BODY" | jq -e '.error.message | test("malformed") | not' >/dev/null 2>&1; then
	_pass "0.0.0.0 rejection does not claim a malformed quad"
else
	_fail "0.0.0.0 rejection message" "still reports a syntax error: $BODY"
fi
# A genuinely malformed selector still reports malformed.
BODY=$(curl -s -X POST "${H_AUTH[@]}" "$API/servers/by-address/not-an-ip:4242/connect")
if printf '%s' "$BODY" | jq -e '.error.message | test("malformed")' >/dev/null 2>&1; then
	_pass "a malformed selector still reports malformed"
else
	_fail "malformed selector message" "expected a malformed-selector error, got: $BODY"
fi
# Positive path: only test if there's actually a server in the cache.
# The address field is the canonical "<ip-or-hostname>:<port>" string
# the daemon reports, so we use it verbatim (no need to convert the
# numeric `ip` field — `address` is the operator-meaningful form).
ADDR=$(curl -s "${H_AUTH[@]}" "$API/servers" \
	| jq -r '.servers[0].address // empty')
if [ -n "$ADDR" ] && [ "$ADDR" != "null" ]; then
	RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
		"$API/servers/by-address/$ADDR/connect")
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
	"$API/logs/amule")
if [ "$RC" = "204" ]; then
	_pass "DELETE /logs/amule → 204"
else
	_fail "logs/amule DELETE" "expected 204, got $RC"
fi
# Fast GET immediately after — must show empty / post-reset state.
GET_BODY=$(curl -s "${H_AUTH[@]}" "$API/logs/amule")
LINES=$(echo "$GET_BODY" | jq -r '.lines | length' 2>/dev/null)
TOTAL=$(echo "$GET_BODY" | jq -r '.total_lines' 2>/dev/null)
if [ "$LINES" = "0" ] && [ "$TOTAL" = "0" ]; then
	_pass "GET /logs/amule immediately after DELETE returns empty (no stale cache)"
else
	_fail "logs/amule post-DELETE freshness" \
		"lines=$LINES total=$TOTAL — expected 0/0"
fi

# --- 6. DELETE /logs/server_info + freshness (lazy cache!). -------
RC=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "${H_AUTH[@]}" \
	"$API/logs/server_info")
if [ "$RC" = "204" ]; then
	_pass "DELETE /logs/server_info → 204"
else
	_fail "logs/server_info DELETE" "expected 204, got $RC"
fi
# Fast GET immediately after — must show empty / post-reset.
GET_BODY=$(curl -s "${H_AUTH[@]}" "$API/logs/server_info")
BYTES=$(echo "$GET_BODY" | jq -r '.total_bytes' 2>/dev/null)
if [ "$BYTES" = "0" ]; then
	_pass "GET /logs/server_info immediately after DELETE returns empty (lazy cache invalidated)"
else
	_fail "logs/server_info post-DELETE freshness" \
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
	local present
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
		present=$(curl -s "${H_AUTH[@]}" "$API/downloads" \
			| jq -r --arg h "$TEST_HASH" \
				'.downloads | map(select(.hash == $h)) | length')
		if [ "$present" = "0" ]; then return 0; fi
		sleep 1
	done
	return 1
}
curl -s -X DELETE "${H_AUTH[@]}" "$API/downloads/$TEST_HASH" > /dev/null
_wait_for_no_download || true

# Array form
RC=$(curl -s -o /tmp/p11_dl.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d "{\"links\":[\"$TEST_LINK\"]}" "$API/downloads")
if [ "$RC" = "202" ]; then
	_pass "POST /downloads array form → 202"
else
	_fail "downloads array status" "expected 202, got $RC: $(cat /tmp/p11_dl.json)"
fi
# Per-item results envelope (#358): one accepted result.
if jq -e '(.results | length) == 1 and .results[0].ok == true' /tmp/p11_dl.json >/dev/null 2>&1; then
	_pass "POST /downloads array reports one ok result"
else
	_fail "downloads array results" "$(cat /tmp/p11_dl.json)"
fi
# `links` is the only spelling. The singular `ed2k_link` is refused
# rather than ignored, with or without `links` alongside it, and the message
# names the array form so a caller sending it is told where to go.
for BODY in \
	"{\"ed2k_link\":\"$TEST_LINK\",\"links\":[\"$TEST_LINK\"]}" \
	"{\"ed2k_link\":\"$TEST_LINK\"}"; do
	RC=$(curl -s -o /tmp/p11_dl_alias.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
		-H "Content-Type: application/json" -d "$BODY" "$API/downloads")
	if [ "$RC" = "400" ]; then
		_pass "POST /downloads with ed2k_link → 400"
	else
		_fail "downloads ed2k_link body" "expected 400, got $RC"
	fi
done
if jq -e '.error.message | test("`links`")' /tmp/p11_dl_alias.json >/dev/null 2>&1; then
	_pass 'the ed2k_link 400 names `links` as the replacement'
else
	_fail "ed2k_link 400 message" "$(cat /tmp/p11_dl_alias.json)"
fi

# --- 8. POST /networks/disconnect selector. ----------------------
# Default (no body) = both
RC=$(curl -s -o /tmp/p11_nd.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	"$API/networks/disconnect")
if [ "$RC" = "202" ]; then
	_pass "POST /networks/disconnect (no body) → 202 default=both"
else
	_fail "networks disconnect no-body" "expected 202, got $RC"
fi
sleep 2
# selector=ed2k
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"network":"ed2k"}' "$API/networks/disconnect")
if [ "$RC" = "202" ]; then
	_pass "POST /networks/disconnect {\"network\":\"ed2k\"} → 202"
else
	_fail "networks disconnect ed2k" "expected 202, got $RC"
fi
sleep 1
# selector=kad
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"network":"kad"}' "$API/networks/disconnect")
if [ "$RC" = "202" ]; then
	_pass "POST /networks/disconnect {\"network\":\"kad\"} → 202"
else
	_fail "networks disconnect kad" "expected 202, got $RC"
fi
# Invalid selector
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"network":"icq"}' "$API/networks/disconnect")
if [ "$RC" = "400" ]; then
	_pass "POST /networks/disconnect bogus selector → 400"
else
	_fail "networks disconnect bogus" "expected 400, got $RC"
fi

# --- 9. /clients filter. -----------------------------------------
# Get baseline first
TOTAL_CLIENTS=$(curl -s "${H_AUTH[@]}" "$API/clients" \
	| jq '.clients | length')
UP_CLIENTS=$(curl -s "${H_AUTH[@]}" "$API/clients?activity=uploading" \
	| jq '.clients | length')
DOWN_CLIENTS=$(curl -s "${H_AUTH[@]}" "$API/clients?activity=downloading" \
	| jq '.clients | length')
ACTIVE_CLIENTS=$(curl -s "${H_AUTH[@]}" "$API/clients?activity=active" \
	| jq '.clients | length')
if [ "$UP_CLIENTS" -le "$TOTAL_CLIENTS" ] 2>/dev/null \
   && [ "$DOWN_CLIENTS" -le "$TOTAL_CLIENTS" ] 2>/dev/null \
   && [ "$ACTIVE_CLIENTS" -le "$TOTAL_CLIENTS" ] 2>/dev/null; then
	_pass "/clients activity: total=$TOTAL_CLIENTS up=$UP_CLIENTS down=$DOWN_CLIENTS active=$ACTIVE_CLIENTS (each ≤ total)"
else
	_fail "clients activity sizing" \
		"total=$TOTAL_CLIENTS up=$UP_CLIENTS down=$DOWN_CLIENTS active=$ACTIVE_CLIENTS"
fi
# `active` = |uploading ∪ downloading|, so it sits in
# [max(up, down), up+down]. The lower bound holds because every
# uploading-or-downloading-only client is in active; the upper bound holds
# because a client simultaneously in both states (upload_state=uploading
# AND download_state=downloading) gets counted once in active but
# twice in (up + down). Exact equality is intentionally not asserted
# because the intersection count is unobservable from activity results
# alone.
EXP_UPPER=$((UP_CLIENTS + DOWN_CLIENTS))
if [ "$ACTIVE_CLIENTS" -ge "$UP_CLIENTS" ] 2>/dev/null \
   && [ "$ACTIVE_CLIENTS" -ge "$DOWN_CLIENTS" ] 2>/dev/null \
   && [ "$ACTIVE_CLIENTS" -le "$EXP_UPPER" ] 2>/dev/null; then
	_pass "/clients?activity=active sits in [max(up,down), up+down]"
else
	_fail "clients active span" \
		"active=$ACTIVE_CLIENTS up=$UP_CLIENTS down=$DOWN_CLIENTS (expected max..sum)"
fi
# Verify every entry in /clients?activity=uploading truly has upload_state=uploading
BAD=$(curl -s "${H_AUTH[@]}" "$API/clients?activity=uploading" \
	| jq -r '.clients[] | select(.upload_state != "uploading") | .ecid' \
	| head -1)
if [ -z "$BAD" ]; then
	_pass "/clients?activity=uploading only returns upload_state=uploading clients"
else
	_fail "clients activity=uploading content" \
		"client_ecid $BAD has wrong upload_state"
fi
# Bogus filter → 400
RC=$(curl -s -o /dev/null -w "%{http_code}" "${H_AUTH[@]}" \
	"$API/clients?activity=alphabetical")
if [ "$RC" = "400" ]; then
	_pass "/clients?activity=<bogus> → 400"
else
	_fail "clients bogus activity" "expected 400, got $RC"
fi

# --- 9a-bis. Base transfer-filename fields on the LIST object -----
# (PR #646 / issue #115). upload_file_name (the partfile a peer is
# downloading FROM us) and download_file_name (the name the peer
# advertised) are both part of the base client field set, so they must
# appear on every /clients LIST object -- not only /clients/{ecid}.
# This keeps an upload-only peer's File column in the WebUI populated, since
# upload_file_name is on the LIST object and not only the detail endpoint.
# Guarded on a live peer so the assertion is real (non-vacuous)
# when a peer exists, and skips cleanly when none are connected.
FIRST_CLIENT=$(curl -s "${H_AUTH[@]}" "$API/clients" | jq -r '.clients[0] // empty')
if [ -n "$FIRST_CLIENT" ]; then
	if curl -s "${H_AUTH[@]}" "$API/clients" | jq -e '
		.clients | all(.[];
			((.upload_file_name | type) as $t | $t == "string" or $t == "null")
			and ((.download_file_name | type) as $t | $t == "string" or $t == "null"))' \
		>/dev/null 2>&1; then
		_pass "/clients list objects carry upload_file_name + download_file_name"
	else
		_fail "clients base filename fields" \
			"a /clients object is missing upload_file_name/download_file_name or it is neither string nor null"
	fi
else
	_skip "/clients base filename-field check (no peers connected)"
fi

# --- 9a-ter. friends-list flag on the LIST object (issue #1475) ---
# friend is friends-list membership and used to be detail-only, which left a
# front-end unable to mark a friend in the peer lists. It is a plain boolean,
# never null: an absent EC tag degrades to false. friend_slot is checked
# alongside it because the two are easy to confuse and carry different meanings.
# Read-only, so it is safe here rather than at the end of the file.
if [ -n "$FIRST_CLIENT" ]; then
	if curl -s "${H_AUTH[@]}" "$API/clients" | jq -e '
		.clients | all(.[];
			(.friend | type) == "boolean"
			and (.friend_slot | type) == "boolean")' \
		>/dev/null 2>&1; then
		_pass "/clients list objects carry friend + friend_slot booleans"
	else
		_fail "clients friend field" \
			"a /clients object is missing friend/friend_slot or it is not a boolean"
	fi
	# credit_ratio moved to the list row with it (issue #1474): the core's modifier,
	# null when the daemon does not send the tag, never re-derived from the totals.
	if curl -s "${H_AUTH[@]}" "$API/clients" | jq -e '
		.clients | all(.[];
			has("credit_ratio")
			and ((.credit_ratio | type) == "number" or .credit_ratio == null)
			and (.credit_ratio == null or (.credit_ratio >= 1 and .credit_ratio <= 10)))' \
		>/dev/null 2>&1; then
		_pass "/clients list objects carry credit_ratio (number in [1,10] or null)"
	else
		_fail "clients credit_ratio field" \
			"a /clients object is missing credit_ratio, has the wrong type, or is out of range"
	fi
else
	_skip "/clients friend-field check (no peers connected)"
fi

# --- 9b. /clients/{ecid} detail (issue #422). --------------------
# A superset of the list object: every list field plus the detail-only
# B fields. Guarded on a live peer; the negative cases run regardless.
FIRST_ECID=$(curl -s "${H_AUTH[@]}" "$API/clients" \
	| jq -r '.clients[0].ecid // empty')
if [ -n "$FIRST_ECID" ]; then
	DETAIL=$(curl -s "${H_AUTH[@]}" "$API/clients/$FIRST_ECID")
	OK=$(echo "$DETAIL" | jq -r --argjson e "$FIRST_ECID" '
		(.ecid == $e)
		and has("user_hash")
		and has("upload_file_name") and has("download_file_name")
		and has("ed2k_user_id") and has("high_id")
		and has("server_ip") and has("server_port") and has("server_name")
		and has("kad_port") and has("source_origin")
		and has("parts_offered_count") and has("client_mod_name")
		and has("shared_files_browsable")
		and has("friend") and has("credit_ratio")')
	if [ "$OK" = "true" ]; then
		_pass "/clients/{ecid} returns detail superset (ecid=$FIRST_ECID)"
	else
		_fail "clients detail shape" "missing detail fields: $DETAIL"
	fi
else
	_skip "/clients/{ecid} detail 200 (no peers connected)"
fi
# Unknown ecid → 404 (0xFFFFFFFF is effectively never a live ECID)
RC=$(curl -s -o /dev/null -w "%{http_code}" "${H_AUTH[@]}" "$API/clients/4294967295")
if [ "$RC" = "404" ]; then
	_pass "/clients/{unknown-ecid} → 404"
else
	_fail "clients detail 404" "expected 404, got $RC"
fi
# Non-numeric ecid path → 400
RC=$(curl -s -o /dev/null -w "%{http_code}" "${H_AUTH[@]}" "$API/clients/not-a-number")
if [ "$RC" = "400" ]; then
	_pass "/clients/{non-numeric} → 400"
else
	_fail "clients detail 400" "expected 400, got $RC"
fi
# Non-GET → 405
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" "$API/clients/1")
if [ "$RC" = "405" ]; then
	_pass "POST /clients/{ecid} → 405"
else
	_fail "clients detail 405" "expected 405, got $RC"
fi

_wait_for_sse() {
	local attempt
	for ((attempt = 0; attempt < 100; ++attempt)); do
		grep -q '^: connected' "$SSE" && return 0
		kill -0 "$PID" 2>/dev/null || break
		sleep 0.1
	done
	_fail "SSE subscription" "stream did not become ready: $(head -10 "$SSE")"
	return 1
}

_wait_for_diff_resume() {
	local attempt
	for ((attempt = 0; attempt < 100; ++attempt)); do
		grep -qE '^event: (resync|download_added|download_updated|status_changed)$' "$SSE" && return 0
		kill -0 "$PID" 2>/dev/null || break
		sleep 0.1
	done
	_fail "SSE diff resume" "no diff tick or resync received: $(head -10 "$SSE")"
	return 1
}

# --- 10. /events ?channels= filter. ------------------------------
# Test 8 left the daemon disconnected. Restore both networks for subsequent
# live phases. The fixture only restarts its contact-free Kad component, which
# accepts an All search without joining the public network.
CONNECT_NETWORK=both
if [ "${1:-}" = "--fixture-ready" ]; then
	CONNECT_NETWORK=kad
fi
RC=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" -d "{\"network\":\"$CONNECT_NETWORK\"}" "$API/networks/connect")
if [ "$RC" = "202" ]; then
	_pass "reconnect $CONNECT_NETWORK → 202"
else
	_fail "reconnect $CONNECT_NETWORK" "expected 202, got $RC"
fi
: > "$SSE"
( curl -s -m 20 -N "${H_AUTH[@]}" \
	"$API/events?channels=downloads,status" \
	>> "$SSE" 2>&1 ) &
PID=$!
_wait_for_sse || true
# The initial ': connected' chunk precedes the first resumed diff tick. After
# suspension that tick primes the baseline and emits resync(idle), which may
# include a mutation made immediately after connecting. Use a distinct probe to
# observe that tick before the mutation whose event we assert below.
RC=$(curl -s -o /tmp/p11_sse_probe.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d "{\"links\":[\"$SSE_PROBE_LINK\"]}" "$API/downloads")
if [ "$RC" = "202" ] && jq -e '.results[0].ok == true' /tmp/p11_sse_probe.json >/dev/null 2>&1; then
	_pass "SSE resume probe accepted"
else
	_fail "SSE resume probe" "expected accepted link, got $RC: $(cat /tmp/p11_sse_probe.json)"
fi
_wait_for_diff_resume || exit 1
# Use a new hash: deleting and re-adding the earlier file between two diff
# ticks can legitimately coalesce into an unchanged snapshot and emit no event.
RC=$(curl -s -o /tmp/p11_sse_download.json -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d "{\"links\":[\"$SSE_LINK\"]}" "$API/downloads")
if [ "$RC" = "202" ] && jq -e '.results[0].ok == true' /tmp/p11_sse_download.json >/dev/null 2>&1; then
	_pass "channel-filter download accepted"
else
	_fail "channel-filter download start" "expected 202 with accepted link, got $RC: $(cat /tmp/p11_sse_download.json)"
fi
sleep 6
kill $PID 2>/dev/null
wait $PID 2>/dev/null
PID=

# In the channels=downloads,status window we MUST see download_* / status_*
# events and MUST NOT see client_* / server_* / shared_* / log_* events.
if awk '/^event: / { download = ($0 == "event: download_added" || $0 == "event: download_updated") }
	download && /^data: / { sub(/^data: /, ""); print }' "$SSE" \
	| jq -es --arg hash "$SSE_HASH" 'any(.[]; .hash == $hash)' >/dev/null 2>&1; then
	_pass "/events?channels=downloads,status delivers download/status events"
else
	_fail "events channel-filter positive" \
		"no event for the accepted download seen; sample: $(head -10 "$SSE")"
fi
# comments_ is in this list on purpose. comments_updated has the prefix
# `comments`, which event_channel does not map, so its channel is literally
# `comments` -- NOT `downloads`, even though the event is a downloads
# concern and the prose reads that way. The channel table documents it as
# its own row; if anyone later folds the prefix into `downloads` without
# updating that table, this assertion is what notices.
LEAKED=$(grep -cE "^event: (client_|server_|shared_|log_|search_|comments_)" "$SSE" || true)
if [ "$LEAKED" -eq 0 ]; then
	_pass "/events?channels=downloads,status excludes client/server/shared/log/search/comments events"
else
	_fail "events channel-filter leak" \
		"$LEAKED off-channel events leaked through"
fi

# Positive: ?channels=search delivers search events and excludes downloads.
: > "$SSE"
( curl -s -m 12 -N "${H_AUTH[@]}" \
	"$API/events?channels=search" \
	>> "$SSE" 2>&1 ) &
PID=$!
_wait_for_sse || true
SEARCH_START=$(mktemp -t amuleapi_26_search_start.XXXXXX)
RC=$(curl -s -o "$SEARCH_START" -w "%{http_code}" -X POST "${H_AUTH[@]}" \
	-H "Content-Type: application/json" \
	-d '{"query":"offlinechannelsregression","type":"all"}' "$API/search")
SEARCH_ID=$(jq -r '.search_id // empty' "$SEARCH_START")
if [ "$RC" = "202" ] && jq -e '.search_id > 0 and .type == "all"' "$SEARCH_START" >/dev/null 2>&1; then
	_pass "channel-filter search accepted with an explicit search_id"
else
	_fail "channel-filter search start" "expected 202 with search_id, got $RC: $(cat "$SEARCH_START")"
fi
rm -f "$SEARCH_START"
sleep 8
kill $PID 2>/dev/null
wait $PID 2>/dev/null
PID=

# Match the newly accepted search, not progress from the fixture's restored one.
if awk '/^event: / { progress = ($0 == "event: search_progress") }
	progress && /^data: / { sub(/^data: /, ""); print }' "$SSE" \
	| jq -es --argjson sid "${SEARCH_ID:-0}" 'any(.[]; .search_id == $sid and .type == "all")' >/dev/null 2>&1; then
	_pass "/events?channels=search delivers search_progress"
else
	_fail "events channel=search positive" \
		"no search_progress in 8 s; sample: $(head -10 "$SSE")"
fi
if [ -n "$SEARCH_ID" ]; then
	curl -s -X POST "${H_AUTH[@]}" "$API/search/$SEARCH_ID/stop" > /dev/null
	curl -s -X DELETE "${H_AUTH[@]}" "$API/search/$SEARCH_ID" > /dev/null
fi
LEAKED=$(grep -cE "^event: (download_|status_|client_|server_|shared_|log_)" "$SSE" || true)
if [ "$LEAKED" -eq 0 ]; then
	_pass "/events?channels=search excludes non-search events"
else
	_fail "events channel=search leak" \
		"$LEAKED off-channel events leaked through"
fi

# --- Summary. -----------------------------------------------------
_summary
