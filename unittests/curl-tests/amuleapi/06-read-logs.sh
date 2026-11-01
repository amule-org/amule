#!/usr/bin/env bash
#
# amuleapi 06-read-logs — /logs/amule + /logs/server_info. amule log
# rides on STAT_REQ's `EC_TAG_STATS_LOGGER_MESSAGE` channel (per-EC-
# connection cursor, incremental); server-info log is full-snapshot
# via `EC_OP_GET_SERVERINFO`.
#
# Bring-up:
#   amuleapi --config-dir=/tmp/amuleapi-06-read-logs ... &
#   ./06-read-logs.sh

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

_preflight

echo "amuleapi 06-read-logs smoke @ $HOST"

TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] || _die "login failed"

# Give the refresher a few seconds to drain the initial log baseline
# (CLoggerAccess emits the entire pre-connection backlog on its first
# tick).
sleep 5

# --- 1. Auth gate. -------------------------------------------------
_curl "$API/logs/amule"
_assert_status 401 "GET /logs/amule (no creds) → 401"
_curl "$API/logs/server_info"
_assert_status 401 "GET /logs/server_info (no creds) → 401"

# --- 2. /logs/amule full response shape. ---------------------------
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/amule"
_assert_status 200 "GET /logs/amule → 200"
_assert_json_eq '.lines | type'         array  '/logs/amule.lines is an array'
_assert_json_eq '.total_lines | type'  number '/logs/amule.total_lines is numeric'
_assert_json_eq '.returned_lines | type'      number '/logs/amule.returned_lines is numeric'
_assert_json_eq '.lines | length > 0'   true   '/logs/amule.lines is non-empty (amule emits a banner on connect)'
# When no tail is given, returned_lines == total_lines.
_assert_json_eq '(.returned_lines == .total_lines)' true \
	'/logs/amule: returned_lines == total_lines when no ?tail given'

TOTAL=$(printf '%s' "$CURL_BODY" | jq '.total_lines')

# --- 3. /logs/amule?tail=N. ----------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/amule?tail=2"
_assert_status 200 "GET /logs/amule?tail=2 → 200"
_assert_json_eq '.returned_lines'           2      '/logs/amule?tail=2 returns 2 lines'
_assert_json_eq '.lines | length'     2      '/logs/amule?tail=2 array length is 2'
_assert_json_eq '.total_lines'       "$TOTAL" '/logs/amule?tail=2 still reports the full line count'

# tail=0 means "no tailing" → all lines. The wire contract makes 0
# the inert default (matches the helper's behaviour: 0 = unbounded).
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/amule?tail=0"
_assert_status 200 "GET /logs/amule?tail=0 → 200"
_assert_json_eq '(.returned_lines == .total_lines)' true \
	'/logs/amule?tail=0 returns all (tail=0 is "no tailing")'

# A non-numeric tail is a 400, not a silent "return all": the count is
# validated like every other on the surface, so a typo is reported rather than
# quietly handing back the whole buffer.
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/amule?tail=notanumber"
_assert_status 400 "GET /logs/amule?tail=notanumber → 400"

# Out of range is the same answer: a 400, not a clamp.
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/amule?tail=999999"
_assert_status 400 "GET /logs/amule?tail=999999 → 400"

# --- 4. /logs/server_info shape. ------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/server_info"
_assert_status 200 "GET /logs/server_info → 200"
_assert_json_eq '.text | type'           string '/logs/server_info.text is string'
_assert_json_eq '.total_bytes | type'    number '/logs/server_info.total_bytes is numeric'
_assert_json_eq '.returned_bytes | type' number '/logs/server_info.returned_bytes is numeric'
_assert_json_eq '(.returned_bytes == .total_bytes)' true \
	'/logs/server_info: returned_bytes == total_bytes when no ?tail given'

TOTAL_BYTES=$(printf '%s' "$CURL_BODY" | jq '.total_bytes')

# --- 5. /logs/server_info?tail=3 — line-boundary slicing. -----------
_curl -H "Authorization: Bearer $TOKEN" "$API/logs/server_info?tail=3"
_assert_status 200 "GET /logs/server_info?tail=3 → 200"
_assert_json_eq '(.returned_bytes <= .total_bytes)' true \
	'/logs/server_info?tail=3: returned_bytes <= total_bytes'
_assert_json_eq '.total_bytes' "$TOTAL_BYTES" \
	'/logs/server_info?tail=3 reports the same total_bytes'

# --- 6. Method gate. -----------------------------------------------
# DELETE on /logs/{amule,serverinfo} clears the buffer. PATCH is a 405:
# the logs are read+reset only, never partially mutable.
for ep in logs/amule logs/server_info; do
	_curl -X PATCH -H "Authorization: Bearer $TOKEN" "$API/$ep"
	_assert_status 405 "PATCH /api/v1/$ep → 405"
done

# --- Summary. -----------------------------------------------------
_summary
