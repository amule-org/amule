#!/usr/bin/env bash
#
# amuleapi 03-read-status — read endpoints, /status only. Validates the
# refresher → state cache → handler chain end-to-end against a live
# amuled. The remaining 12 endpoints (downloads, uploads, shared,
# servers, kad, categories, logs/amule, logs/server_info, preferences,
# stats/tree, stats/graphs, search/results) land in subsequent
# sub-phases (4b/4c/4d); their phase scripts share this directory.
#
# Bring-up convention:
#   rm -rf /tmp/amuleapi-03-read-status && mkdir -p /tmp/amuleapi-03-read-status
#   amuleapi --config-dir=/tmp/amuleapi-03-read-status --host=127.0.0.1 \
#            --port=4712 --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-03-read-status --host=127.0.0.1 \
#            --port=4712 --password=amule &
#   ./03-read-status.sh
#
# Environment:
#   HOST=localhost:4713          amuleapi endpoint
#   ADMIN_PASS=adminpass         plaintext admin password
#   GUEST_PASS=guestpass         plaintext guest password (run-all.sh
#                                configures it via --set-guest-pass;
#                                standalone invocations need this set
#                                or the guest-read assertion is skipped)
#
# Exits 0 on success, 1 on assertion failure, 2 on bring-up error.

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

_preflight

echo "amuleapi 03-read-status smoke @ $HOST"

# --- 1. /status without auth → 401 unauthorized. -------------------
_curl "$API/status"
_assert_status 401 "GET /api/v1/status (no creds) → 401"
_assert_json_eq '.error.code' unauthorized \
	'unauthenticated /status carries error.code=unauthorized'

# --- 2. Log in as admin and capture the bearer. --------------------
TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] \
	|| _die "could not log in for /status tests"

# Wait for the refresher to land its first snapshot. On a fast
# Linux host the first tick is sub-second; on the Windows VM the
# 503 ec_unavailable window can stretch a few seconds under load.
# Poll up to 15 s, same shape run-all.sh uses for /health, so
# the test doesn't drift to "disconnected" under runner pressure.
for _ in $(seq 1 30); do
	probe=$(curl -s -o /dev/null -w "%{http_code}" \
		-H "Authorization: Bearer $TOKEN" \
		"$API/status")
	[ "$probe" = "200" ] && break
	sleep 0.5
done

# --- 3. /status with bearer → 200 + envelope shape. ----------------
_curl -H "Authorization: Bearer $TOKEN" "$API/status"
_assert_status 200 "GET /api/v1/status (admin bearer) → 200"

# Envelope metadata.
_assert_json_eq '.ec_connected | type' boolean \
	'ec_connected is boolean'

# ed2k subtree.
_assert_json_eq '.ed2k.state | test("^(connected|connecting|disconnected)$")' \
	true 'ed2k.state is a known enum value'
_assert_json_eq '.ed2k.high_id | type' boolean \
	'ed2k.high_id is boolean'
_assert_json_eq '.ed2k.user_id | type' number \
	'ed2k.user_id is numeric'
# `user_id` is the server-assigned identity, not a local handle.
_assert_json_eq '.ed2k | has("id")' false \
	'ed2k exposes user_id, not id'
_assert_json_eq '.ed2k.public_ip | type | . == "string" or . == "null"' true \
	'ed2k.public_ip is a string or null'
# A public address exists exactly when we hold a HighID on a live connection;
# a LowID carries none, and neither does a disconnected daemon. Absent is null,
# not "" -- the surface spells "no value" one way (R10).
_assert_json_eq '(.ed2k.public_ip != null) == (.ed2k.high_id and .ed2k.state == "connected")' \
	true 'ed2k.public_ip is non-null exactly when high_id and connected'
# The 0xffffffff "connect in flight" sentinel must never surface.
_assert_json_eq '.ed2k.user_id != 4294967295' true \
	'ed2k.user_id never reports the connecting sentinel'
# The connected-server triple nulls together, so server_name is asserted
# against server_ip rather than by type alone.
_assert_json_eq '(.ed2k.server_name == null) == (.ed2k.server_ip == null)' true \
	'ed2k.server_name is null exactly when server_ip is'
_assert_json_eq '.ed2k.server_name | type | test("^(string|null)$")' true \
	'ed2k.server_name is a string or null, never another type'

# kad subtree.
_assert_json_eq '.kad.state | test("^(connected|connecting|disabled)$")' \
	true 'kad.state is a known enum value'
# Named for the transport: this is the TCP half of the pair GET /kad reports,
# not an overall verdict refined by firewalled_udp.
# Gated on Kad being connected: the underlying connstate bit outlives a
# disconnect, so this reported a reachability verdict for a network the daemon
# was not on. Same rule as /kad's own copy of the field.
_assert_json_eq '(.kad.state == "connected") or (.kad.firewalled_tcp == null)' true \
	'kad.firewalled_tcp is null while Kad is not connected'
_assert_json_eq '(.kad.state != "connected") or ((.kad.firewalled_tcp | type) == "boolean")' true \
	'kad.firewalled_tcp is boolean while Kad is connected'
# The network rollups, both sides. ed2k's summed the whole known SERVER LIST
# rather than the attached server and nothing zeroed them, so a disconnected
# daemon repeated its connected figures verbatim and indefinitely.
for P in "ed2k user_count" "ed2k file_count" "kad user_count" "kad file_count" "kad node_count"; do
	set -- $P
	_assert_json_eq "(.$1.state == \"connected\") or (.$1.network.$2 == null)" true \
		"$1.network.$2 is null while $1 is not connected"
	_assert_json_eq "(.$1.state != \"connected\") or ((.$1.network.$2 | type) == \"number\")" true \
		"$1.network.$2 is numeric while $1 is connected"
done
_assert_json_eq '.kad | has("firewalled")' false \
	'kad exposes firewalled_tcp, not firewalled'

# speeds + queue subtrees.
_assert_json_eq '.speeds.download_speed_bytes_per_second | type' number \
	'speeds.download_speed_bytes_per_second is numeric'
_assert_json_eq '.speeds.upload_speed_bytes_per_second | type' number \
	'speeds.upload_speed_bytes_per_second is numeric'
_assert_json_eq '.speeds.download_overhead_bytes_per_second | type' number \
	'speeds.download_overhead_bytes_per_second is numeric'
_assert_json_eq '.speeds.upload_overhead_bytes_per_second | type' number \
	'speeds.upload_overhead_bytes_per_second is numeric'
_assert_json_eq '.queue.waiting_upload_client_count | type' number \
	'queue.waiting_upload_client_count is numeric'
_assert_json_eq '.queue.download_source_count | type' number \
	'queue.download_source_count is numeric'

# disk subtree: a number, or null when the daemon has no figure. Never the
# unsigned reading of the -1 sentinel, and never 0 (which would read as a
# full disk).
_assert_json_eq '.disk.temp_free_bytes | type | . == "number" or . == "null"' true \
	'disk.temp_free_bytes is a number or null'
_assert_json_eq '.disk.incoming_free_bytes | type | . == "number" or . == "null"' true \
	'disk.incoming_free_bytes is a number or null'
_assert_json_eq '[.disk[]] | map(select(. == 18446744073709551615)) | length == 0' true \
	'disk figures never report the unsigned free-space sentinel'

# These spellings are not part of the surface; assert they are absent from the
# whole body rather than merely unused by the assertions above.
_assert_json_eq '[paths | join(".")] | map(select(test("low_id|upload_queue_length|total_source_count"))) | length == 0' \
	true '/status carries none of these field names'

# --- 4. /status with guest bearer also works (any-role read gate). --
GUEST_TOKEN=$(_login "$GUEST_PASS")
if [ -n "$GUEST_TOKEN" ]; then
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$API/status"
	_assert_status 200 "GET /api/v1/status (guest bearer) → 200"
else
	# run-all.sh always configures a guest password; if a future
	# fixture drops it, surface the gap rather than silently
	# pretending the guest read-gate was exercised.
	_die "guest login failed in 03-read-status fixture — 03-read-status is supposed to verify both roles can read /status; check that GUEST_PASS is wired"
fi

# --- 5. Method gate. ----------------------------------------------
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$API/status"
_assert_status 405 "DELETE /api/v1/status → 405 method_not_allowed"

# --- 6. HEAD /status. ----------------------------------------------
_curl -I -H "Authorization: Bearer $TOKEN" "$API/status"
_assert_status 200 "HEAD /api/v1/status → 200"

# --- Summary. -----------------------------------------------------
_summary
