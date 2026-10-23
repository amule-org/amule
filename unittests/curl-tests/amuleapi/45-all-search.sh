#!/usr/bin/env bash
# Offline REST/SSE coverage for discovered and API-started All searches.
# Unlike 19-search.sh, this phase needs no public eD2k/Kad connectivity.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" != "--fixture-ready" ]; then
	: "${AMULEAPI_BIN:?Run through run-all.sh or set AMULEAPI_BIN}"
	AMULED_BIN=${AMULED_BIN:-$(dirname "$AMULEAPI_BIN")/../amuled}
	exec python3 "$SCRIPT_DIR/../../tests/AllSearchApiSmoke.py" \
		"$AMULED_BIN" "$AMULEAPI_BIN" "$SCRIPT_DIR/45-all-search.sh"
fi

TMP=$(mktemp -d -t amuleapi_all_smoke.XXXXXX)
SSE_PID=
cleanup() {
	if [ -n "$SSE_PID" ]; then
		kill "$SSE_PID" 2>/dev/null || true
		wait "$SSE_PID" 2>/dev/null || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
TOKEN=$(curl -fsS --max-time 10 -X POST -H 'Content-Type: application/json' \
	-d '{"password":"adminpass"}' "$API/auth/login?include_token=true" | jq -er .token)
AUTH="Authorization: Bearer $TOKEN"
curl -fsS -N --max-time 20 -D "$TMP/headers" -H "$AUTH" \
	"$API/events" >"$TMP/events" 2>"$TMP/curl-error" &
SSE_PID=$!
# Wait for the subscription before creating the session's search-results slot.
for _ in $(seq 1 100); do
	if grep -qis 'content-type: text/event-stream' "$TMP/headers"; then break; fi
	kill -0 "$SSE_PID" 2>/dev/null || fail 'SSE connection exited'
	sleep 0.1
done
grep -qi 'content-type: text/event-stream' "$TMP/headers" || fail 'SSE did not subscribe'
curl -fsS --max-time 10 -H "$AUTH" "$API/search" >"$TMP/searches"
jq -e --argjson sid "$ALL_SID" \
	'any(.searches[]; .search_id == $sid and .type == "all")' "$TMP/searches" >/dev/null \
	|| fail 'GET /search did not report type all'
echo 'PASS: GET /search reports type all'
# The fixture has no trailing parameter block: no form was recorded for it, so the
# row carries no form fields -- the three numeric caps as null, the strings omitted.
jq -e --argjson sid "$ALL_SID" \
	'any(.searches[]; .search_id == $sid and (.file_type | not) and (.extension | not)
		and .min_size_bytes == null and .max_size_bytes == null
		and .min_source_count == null)' "$TMP/searches" >/dev/null \
	|| fail 'GET /search gave a restored search without a block a form it never had'
echo 'PASS: GET /search leaves a pre-block restored search without a form'
curl -fsS --max-time 10 -H "$AUTH" "$API/search/$ALL_SID/results" >"$TMP/results"
echo 'PASS: GET All search results'
# Starts are capability-gated, and All can use Kad alone in this offline fixture.
curl -fsS --max-time 10 -H "$AUTH" "$API/status" | jq -e '.search_all_supported == true' >/dev/null \
	|| fail 'All capability missing from status'
echo 'PASS: status advertises All capability'
STATUS=$(curl -sS --max-time 10 -o "$TMP/more" -w '%{http_code}' \
	-X POST -H "$AUTH" "$API/search/$ALL_SID/more")
[ "$STATUS" = 400 ] || fail "more on restored, finished All returned $STATUS"
echo 'PASS: finished All cannot be extended'
curl -fsS --max-time 10 -X POST -H "$AUTH" -H 'Content-Type: application/json' \
	-d '{"network":"kad"}' "$API/networks/connect" >/dev/null
STATUS=$(curl -sS --max-time 10 -o "$TMP/start" -w '%{http_code}' \
	-X POST -H "$AUTH" -H 'Content-Type: application/json' \
	-d '{"query":"offlineallregression","type":"all","file_type":"archive","extension":"zip","min_size_bytes":1048576,"max_size_bytes":5242880,"min_source_count":2}' "$API/search")
[ "$STATUS" = 202 ] || fail "POST All returned $STATUS: $(cat "$TMP/start")"
SID=$(jq -er '.search_id' "$TMP/start")
jq -e '.type == "all" and .state == "running"' "$TMP/start" >/dev/null \
	|| fail 'POST All response has wrong type/state'
# The creation echoes the form in the row vocabulary: the daemon recorded the same
# parameters the client sent, so the row carries them back like GET /search will.
jq -e '.file_type == "archive" and .extension == "zip"
	and .min_size_bytes == 1048576 and .max_size_bytes == 5242880
	and .min_source_count == 2' "$TMP/start" >/dev/null \
	|| fail 'POST All response did not echo the submitted form'
echo 'PASS: POST All starts using Kad fallback and echoes its form'
curl -fsS --max-time 10 -H "$AUTH" "$API/search" >"$TMP/searches"
jq -e --argjson sid "$SID" \
	'any(.searches[]; .search_id == $sid and .file_type == "archive"
		and .extension == "zip" and .min_size_bytes == 1048576
		and .max_size_bytes == 5242880 and .min_source_count == 2)' "$TMP/searches" >/dev/null \
	|| fail 'GET /search did not report the submitted form back'
echo 'PASS: GET /search reports the form a search was started with'
STATUS=$(curl -sS --max-time 10 -o "$TMP/more" -w '%{http_code}' \
	-X POST -H "$AUTH" "$API/search/$SID/more")
[ "$STATUS" = 202 ] || fail "more on active All returned $STATUS: $(cat "$TMP/more")"
echo 'PASS: active Kad component of All can be extended'
curl -fsS --max-time 10 -H "$AUTH" "$API/search/$SID/results" \
	| jq -e '.progress.type == "all" and .progress.kad_active == true' >/dev/null \
	|| fail 'All progress did not report active Kad component'
echo 'PASS: All results report component activity'
STATUS=$(curl -sS --max-time 10 -o "$TMP/stop" -w '%{http_code}' \
	-X POST -H "$AUTH" "$API/search/$SID/stop")
[ "$STATUS" = 204 ] || fail "stop All returned $STATUS"
STATUS=$(curl -sS --max-time 10 -o "$TMP/more" -w '%{http_code}' \
	-X POST -H "$AUTH" "$API/search/$SID/more")
[ "$STATUS" = 400 ] || fail "more on stopped All returned $STATUS"
echo 'PASS: stopped All cannot be extended'
STATUS=$(curl -sS --max-time 10 -o "$TMP/delete" -w '%{http_code}' \
	-X DELETE -H "$AUTH" "$API/search/$SID")
[ "$STATUS" = 204 ] || fail "delete API-started All returned $STATUS"
echo 'PASS: delete API-started All'
wait "$SSE_PID" || true
SSE_PID=
awk '/^event: / { progress = ($0 == "event: search_progress") }
	progress && /^data: / { sub(/^data: /, ""); print }' "$TMP/events" \
	| jq -es --argjson sid "$ALL_SID" \
	'any(.[]; .search_id == $sid and .type == "all")' >/dev/null \
	|| fail 'SSE search_progress did not report type all'
echo 'PASS: SSE search_progress reports type all'
STATUS=$(curl -sS --max-time 10 -o "$TMP/delete" -w '%{http_code}' \
	-X DELETE -H "$AUTH" "$API/search/$ALL_SID")
[ "$STATUS" = 204 ] || fail "DELETE All search returned $STATUS"
echo 'PASS: DELETE All search (HTTP 204)'
echo 'OK: 11/11 passed (no skips)'
