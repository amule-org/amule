#!/usr/bin/env bash
#
# amuleapi 05-read-servers-kad-categories-prefs — /servers, /kad, /categories, /preferences.
# Exercises the four endpoints added in this sub-phase. Tolerant of
# empty caches (e.g. amuled with no configured servers) — asserts
# envelope shape + per-item field types without requiring specific
# content.
#
# Bring-up:
#   amuleapi --config-dir=/tmp/amuleapi-05-read-servers-kad-categories-prefs \
#            --host=127.0.0.1 --port=4712 --password=amule \
#            --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-05-read-servers-kad-categories-prefs ... &
#   ./05-read-servers-kad-categories-prefs.sh

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_05_read_servers_kad_categories_prefs_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	local resp
	resp=$(curl -s --max-time 10 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

_assert_json_eq() {
	local expr=$1 expected=$2 label=$3
	local actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null) \
		|| _fail "$label" "body was not valid JSON" "body: $CURL_BODY"
	if [ "$actual" = "$expected" ]; then
		_pass "$label"
	else
		_fail "$label" "expected $expected, got $actual" "body: $CURL_BODY"
	fi
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 05-read-servers-kad-categories-prefs smoke @ $HOST"

# Log in.
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Wait for the first full refresher tick (servers + prefs land at the
# end of the tick — we need the second tick to confirm steady-state).
sleep 3

# --- 1. /servers ---------------------------------------------------
_curl "$HOST/api/v0/servers"
_assert_status 401 "GET /servers (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/servers"
_assert_status 200 "GET /servers (admin) → 200"
_assert_json_eq '.servers | type'          array  '/servers .servers is an array'
COUNT=$(printf '%s' "$CURL_BODY" | jq '.servers | length')
if [ "$COUNT" -gt 0 ]; then
	echo "  --- /servers has $COUNT entry/entries; per-item shape ---"
	_assert_json_eq '.servers[0].name     | type' string  '/servers[0].name is string'
	_assert_json_eq '.servers[0].address  | type' string  '/servers[0].address is string'
	# Added beside `address`: every client had to re-parse the ip:port form.
	_assert_json_eq '.servers[0].ip       | type' string  '/servers[0].ip is string'
	_assert_json_eq '.servers[0] as $s | ($s.address | startswith($s.ip))' true \
		'/servers[0].address begins with the bare ip'
	_assert_json_eq '.servers[0].port     | type' number  '/servers[0].port is numeric'
	_assert_json_eq '.servers[0].user_count | type' number  '/servers[0].user_count is numeric'
	_assert_json_eq '.servers[0].priority | test("^(low|normal|high)$")' \
		true '/servers[0].priority is a known enum value'
	_assert_json_eq '.servers[0].permanent | type' boolean '/servers[0].permanent is boolean'
	# #440 server country: always-present ISO 3166-1 alpha-2 string,
	# empty when GeoIP is off/unresolved (never absent/null).
	# Nullable since the R10 pass, same as the client row.
	_assert_json_eq '(.servers[0].country_code == null or (.servers[0].country_code | type) == "string")' \
		true '/servers[0].country_code is a string or null'
	# Consecutive failed connection attempts -- a counter, not a boolean,
	# which is why the key is not just "failed".
	_assert_json_eq '.servers[0].failed_count | type' number '/servers[0].failed_count is numeric'
	# Per-user publishing limits. Always present; 0 until the server has
	# answered a UDP status request, so a value is not guaranteed here.
	_assert_json_eq '.servers[0].soft_file_limit | type' number '/servers[0].soft_file_limit is numeric'
	_assert_json_eq '.servers[0].hard_file_limit | type' number '/servers[0].hard_file_limit is numeric'
	# Decoded capability bits: an object carrying the raw bitmask plus one
	# always-present boolean per named bit, so a consumer never has to test
	# for a key or carry the eD2k protocol tables.
	_assert_json_eq '.servers[0].tcp_flags | type'                 object  '/servers[0].tcp_flags is an object'
	_assert_json_eq '.servers[0].tcp_flags.bitmask | type'         number  '/servers[0].tcp_flags.bitmask is numeric'
	_assert_json_eq '.servers[0].tcp_flags.related_search | type'  boolean '/servers[0].tcp_flags.related_search is boolean'
	_assert_json_eq '.servers[0].tcp_flags.tcp_obfuscation | type' boolean '/servers[0].tcp_flags.tcp_obfuscation is boolean'
	_assert_json_eq '.servers[0].udp_flags | type'                 object  '/servers[0].udp_flags is an object'
	_assert_json_eq '.servers[0].udp_flags.bitmask | type'         number  '/servers[0].udp_flags.bitmask is numeric'
	_assert_json_eq '.servers[0].udp_flags.get_sources_v2 | type'  boolean '/servers[0].udp_flags.get_sources_v2 is boolean'
	# The two obfuscation bits are distinct keys in the UDP object; the
	# TCP object spells its own out for the same reason.
	_assert_json_eq '.servers[0].udp_flags.udp_obfuscation | type' boolean '/servers[0].udp_flags.udp_obfuscation is boolean'
	_assert_json_eq '.servers[0].udp_flags.tcp_obfuscation | type' boolean '/servers[0].udp_flags.tcp_obfuscation is boolean'
	# Each decoded boolean must agree with the bitmask it came from.
	_assert_json_eq '.servers[0].tcp_flags | (.bitmask / 64 | floor | . % 2 == 1) == .related_search' \
		true '/servers[0].tcp_flags.related_search matches bit 0x0040 of the bitmask'
fi

# --- 2. /kad -------------------------------------------------------
_curl "$HOST/api/v0/kad"
_assert_status 401 "GET /kad (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/kad"
_assert_status 200 "GET /kad (admin) → 200"
_assert_json_eq '.state | test("^(disabled|connecting|connected)$")' \
	true '/kad.state is a known enum value'
# firewalled_tcp and firewalled_udp are two independent measurements, not a
# verdict and a refinement -- which is what the unqualified `firewalled` used
# to imply. The TCP one is a vote (two peers must confirm reachability over an
# incoming connection); the UDP one is a directed test with its own timeout.
# Typed by connection state now: a measured bool while connected, null while
# not. `false` used to mean both "measured open" and "never measured", and for
# firewalled_udp specifically that read as "UDP is open" on a stopped Kad.
for F in firewalled_tcp firewalled_udp lan_mode; do
	_assert_json_eq "(.state == \"connected\") or (.$F == null)" true \
		"/kad.$F is null while Kad is not connected"
	_assert_json_eq "(.state != \"connected\") or ((.$F | type) == \"boolean\")" true \
		"/kad.$F is boolean while Kad is connected"
done
# The pre-rename spellings must be gone, not merely shadowed by the new ones.
_assert_json_eq 'has("firewalled")'   false '/kad.firewalled is gone'
_assert_json_eq 'has("in_lan_mode")'  false '/kad.in_lan_mode is gone'
# LAN mode forces both firewall flags false (Kademlia.h, UDPFirewallTester.cpp),
# so the three cannot all be true at once.
_assert_json_eq '(.lan_mode | not) or ((.firewalled_tcp | not) and (.firewalled_udp | not))' \
	true '/kad LAN mode forces both firewalled flags false'
# amuled sends the UDP test result only while Kad is connected, so a false
# there is the absence of a measurement rather than "UDP is open". The TCP
# side defaults the other way: true until two peers vouch for us.
# Was: "reads false while not connected". That false was the absence of a
# measurement wearing the costume of one, and it is null now.
_assert_json_eq '.connected_since_at | type' number  '/kad.connected_since_ate is numeric'
# Ours. Named apart from the buddy's address, which the rename must not touch.
_assert_json_eq '.public_ip        | type' string  '/kad.public_ip is string'
_assert_json_eq '.ip               | type' null    '/kad has no bare top-level ip'
# 32 lowercase hex while Kad runs, "" while it does not -- gated on .state,
# which is "disabled" exactly when amuled withholds EC_TAG_KAD_ID.
_assert_json_eq '(.state == "disabled") or (.node_id | test("^[0-9a-f]{32}$"))' \
	true '/kad.node_id is 32 lowercase hex chars while Kad runs'
_assert_json_eq '(.state != "disabled") or (.node_id == "")' \
	true '/kad.node_id is empty while Kad is not running'
# The network rollup and the store counters, both gated on being connected.
# `nodes` is the sharp one: it is this node's OWN routing-table size, and
# contacts outlive a disconnect, so it was measured at 2 on a fully stopped Kad
# -- a non-zero figure for a network the daemon was not on.
for F in network.user_count network.file_count network.node_count \
	indexed.sources indexed.keywords indexed.notes indexed.load_percent; do
	_assert_json_eq "(.state == \"connected\") or (.$F == null)" true \
		"/kad.$F is null while Kad is not connected"
	_assert_json_eq "(.state != \"connected\") or ((.$F | type) == \"number\")" true \
		"/kad.$F is numeric while Kad is connected"
done
# indexed.load_percent is a load figure rather than a count, despite sitting beside
# three counts -- covered by the loop above.
# Two distinct "unknown" sentinels: "" while Kad is not connected, and a
# syntactically valid 0.0.0.0 while connected but not yet told our address.
_assert_json_eq '(.state == "connected") or (.public_ip == "")' \
	true '/kad.public_ip is empty while Kad is not connected'
# Gated with the rest: `no_buddy` is a real state, so reporting it on a stopped
# Kad claimed we had looked and found none. Null when not connected; the enum
# and the types still hold while connected.
for F in buddy.state buddy.ip buddy.port; do
	_assert_json_eq "(.state == \"connected\") or (.$F == null)" true \
		"/kad.$F is null while Kad is not connected"
done
_assert_json_eq '(.state != "connected") or ((.buddy.port | type) == "number")' \
	true '/kad.buddy.port is numeric while Kad is connected'
_assert_json_eq '(.state != "connected") or ((.buddy.ip | type) == "string")' \
	true '/kad.buddy.ip survives the ip rename'
_assert_json_eq '(.state != "connected") or (.buddy.state | test("^(no_buddy|connecting|connected|unknown)$"))' \
	true '/kad.buddy.state is a known enum value while Kad is connected'

# --- 3. /categories -----------------------------------------------
_curl "$HOST/api/v0/categories"
_assert_status 401 "GET /categories (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/categories"
_assert_status 200 "GET /categories (admin) → 200"
_assert_json_eq '.categories | type' array '/categories.categories is an array'
CATCOUNT=$(printf '%s' "$CURL_BODY" | jq '.categories | length')
if [ "$CATCOUNT" -gt 0 ]; then
	echo "  --- /categories has $CATCOUNT entry/entries; per-item shape ---"
	_assert_json_eq '.categories[0].index | type' number '/categories[0].index is numeric'
	_assert_json_eq '.categories[0].name  | type' string '/categories[0].name is string'
	_assert_json_eq '.categories[0].priority | test("^(very_low|low|normal|high|release|auto)$")' \
		true '/categories[0].priority is a known enum value'
fi

# --- 4. /preferences ----------------------------------------------
_curl "$HOST/api/v0/preferences"
_assert_status 401 "GET /preferences (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/preferences"
_assert_status 200 "GET /preferences (admin) → 200"
# Bare object (no envelope) per Q3 — preferences is a single resource.
_assert_json_eq '.snapshot_at | type' null \
	'/preferences has no snapshot_at envelope (bare object)'

_assert_json_eq '.general.nickname             | type' string  '/preferences.general.nickname is string'
_assert_json_eq '.general.user_hash | length'                          32   '/preferences.general.user_hash is 32-char hex'
_assert_json_eq '.general.version_check_enabled    | type' boolean '/preferences.general.version_check_enabled is boolean'

_assert_json_eq '.connection.tcp_port          | type' number  '/preferences.connection.tcp_port is numeric'
_assert_json_eq '.connection.udp_port          | type' number  '/preferences.connection.udp_port is numeric'
_assert_json_eq '.connection.extended_udp_port_enabled | type' boolean '/preferences.connection.extended_udp_port_enabled is boolean (#596)'
_assert_json_eq '.connection.ed2k_enabled      | type' boolean '/preferences.connection.ed2k_enabled is boolean'
_assert_json_eq '.connection.kad_enabled       | type' boolean '/preferences.connection.kad_enabled is boolean'
_assert_json_eq '.connection.autoconnect       | type' boolean '/preferences.connection.autoconnect is boolean'
_assert_json_eq '.connection.max_sources_per_file_count | type' number '/preferences.connection.max_sources_per_file_count is numeric'
# Statistics graph-scale caps were dropped from /preferences (#596).
_assert_json_eq '.connection.max_upload_cap_kbps   | type' null '/preferences.connection.max_upload_cap_kbps removed (#596)'
_assert_json_eq '.connection.max_download_cap_kbps | type' null '/preferences.connection.max_download_cap_kbps removed (#596)'

# 3-state enum string, not a bool (#596, renamed + spelled out in #655);
# endgame newly exposed (#596).
_assert_json_eq '.security.shared_files_visibility | test("^(everybody|friends|nobody)$")' \
	true '/preferences.security.shared_files_visibility is a known 3-state enum value (#655)'
_assert_json_eq '.files.endgame_mode_enabled        | type' boolean '/preferences.files.endgame_mode_enabled is boolean (#596)'
# Old names must be gone, not merely shadowed by the new ones (#655).
_assert_json_eq '.security.can_see_shares      | type' null    '/preferences.security.can_see_shares removed (#655)'
_assert_json_eq '.files.endgame                | type' null    '/preferences.files.endgame removed (#655)'

# message_filter show-in-log + comment filter, wired over EC (#596).
_assert_json_eq '.message_filter.log_filtered_messages      | type' boolean '/preferences.message_filter.log_filtered_messages is boolean (#596)'
_assert_json_eq '.message_filter.filter_comments  | type' boolean '/preferences.message_filter.filter_comments is boolean (#596)'
_assert_json_eq '.message_filter.comment_keywords | type' string  '/preferences.message_filter.comment_keywords is string (#596)'

# geoip config category (#440). Field types are always present even
# on a GeoIP-less daemon (supported=false, strings empty); source is one
# of the known enum values.
_assert_json_eq '.geoip.supported       | type' boolean '/preferences.geoip.supported is boolean'
_assert_json_eq '.geoip.enabled         | type' boolean '/preferences.geoip.enabled is boolean'
_assert_json_eq '.geoip.source | test("^(dbip|maxmind|custom)$")' \
	true '/preferences.geoip.source is a known enum value'
_assert_json_eq '.geoip.custom_update_url      | type' string  '/preferences.geoip.custom_update_url is string'
_assert_json_eq '.geoip.maxmind_license | type' string  '/preferences.geoip.maxmind_license is string'
_assert_json_eq '.geoip.auto_update     | type' boolean '/preferences.geoip.auto_update is boolean'
_assert_json_eq '.geoip.loaded_source   | type' string  '/preferences.geoip.loaded_source is string'
_assert_json_eq '.geoip.db_path         | type' string  '/preferences.geoip.db_path is string'
_assert_json_eq '.geoip.db_loaded       | type' boolean '/preferences.geoip.db_loaded is boolean'
_assert_json_eq '.geoip.download_in_progress | type' boolean '/preferences.geoip.download_in_progress is boolean'
_assert_json_eq '.geoip.last_update_status  | type' string  '/preferences.geoip.last_update_status is string'

# --- Nested remote_controls (#655). ----------------------------
# The two subsystems are sub-objects, not webserver_* / amuleapi_* prefixes.
_assert_json_eq '.remote_controls.webserver        | type' object  '/preferences.remote_controls.webserver is an object (#655)'
_assert_json_eq '.remote_controls.amuleapi         | type' object  '/preferences.remote_controls.amuleapi is an object (#655)'
_assert_json_eq '.remote_controls.webserver.enabled         | type' boolean '/preferences.remote_controls.webserver.enabled is boolean'
_assert_json_eq '.remote_controls.webserver.port            | type' number  '/preferences.remote_controls.webserver.port is numeric'
_assert_json_eq '.remote_controls.webserver.refresh_seconds | type' number  '/preferences.remote_controls.webserver.refresh_seconds is numeric'
_assert_json_eq '.remote_controls.webserver.template_name        | type' string  '/preferences.remote_controls.webserver.template_name is string'
_assert_json_eq '.remote_controls.amuleapi.enabled          | type' boolean '/preferences.remote_controls.amuleapi.enabled is boolean'
_assert_json_eq '.remote_controls.amuleapi.port             | type' number  '/preferences.remote_controls.amuleapi.port is numeric'
_assert_json_eq '.remote_controls.amuleapi.bind_address     | type' string  '/preferences.remote_controls.amuleapi.bind_address is string'
_assert_json_eq '.remote_controls.webserver_enabled | type' null '/preferences.remote_controls.webserver_enabled removed (#655)'
_assert_json_eq '.remote_controls.amuleapi_bind     | type' null '/preferences.remote_controls.amuleapi_bind removed (#655)'
# Passwords stay write-only in both sub-objects.
_assert_json_eq '.remote_controls.webserver.password       | type' null '/preferences.remote_controls.webserver.password is not emitted'
_assert_json_eq '.remote_controls.webserver.guest_password | type' null '/preferences.remote_controls.webserver.guest_password is not emitted'
_assert_json_eq '.remote_controls.amuleapi.password        | type' null '/preferences.remote_controls.amuleapi.password is not emitted'

# proxy_type is an enum string, not a magic number (#655).
_assert_json_eq '.connection.proxy_type | type' string '/preferences.connection.proxy_type is a string (#655)'

# --- Method gate. ----------------------------------------------
for ep in servers kad categories preferences; do
	_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/$ep"
	_assert_status 405 "DELETE /api/v0/$ep → 405"
done

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
