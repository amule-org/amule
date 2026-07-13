#!/usr/bin/env bash
#
# amuleapi 15-preferences-patch — PATCH /preferences.
#
# Endpoint:
#   PATCH /api/v0/preferences
#       body: { general?, connection?, directories?, files?, servers?,
#               security?, message_filter?, remote_controls?,
#               online_signature?, core_tweaks?, kademlia? }  (issue #437)
#
# Wire shape mirrors the /preferences GET response. Both sub-objects
# optional; all fields within optional. Only fields present are
# applied. Returns 200 with the post-mutation /preferences body so
# consumers can confirm what landed without a follow-up GET.
#
# EC packet shape: `EC_OP_SET_PREFERENCES` at `EC_DETAIL_FULL`. FULL
# is required so amuled's CEC_Prefs_Packet::Apply() honors boolean
# tags (it gates ApplyBoolean on `use_tag = (detail == FULL)` per
# ECSpecialMuleTags.cpp:392).
#
# No-stale-cache invariant: PATCH returns the post-mutation state in
# its response body AND the immediate-following GET shows the same
# values. RefresherTick is called inline after every successful
# SET_PREFERENCES roundtrip.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_15_preferences_patch_body.XXXXXX)
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

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 15-preferences-patch smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# Save the pre-mutation state so we can restore everything at the
# end. We only modify two fields (max_upload_kbps + autoconnect) so
# the operator's daemon doesn't end the smoke in an unexpected state.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
SAVED_MAX_UPLOAD=$(printf '%s' "$CURL_BODY" | jq -r '.connection.max_upload_kbps')
SAVED_AUTOCONNECT=$(printf '%s' "$CURL_BODY" | jq -r '.connection.autoconnect')
echo "    info: saved state max_upload_kbps=$SAVED_MAX_UPLOAD autoconnect=$SAVED_AUTOCONNECT"

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kbps":42}}' "$HOST/api/v0/preferences"
_assert_status 401 "PATCH /preferences (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X PATCH -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"connection":{"max_upload_kbps":42}}' "$HOST/api/v0/preferences"
	_assert_status 403 "PATCH /preferences (guest) → 403"
else
	echo "    info: no guest pass; admin-gate skipped"
fi

# --- 2. PATCH numeric field — response + no-stale GET. -------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kbps":42}}' "$HOST/api/v0/preferences"
_assert_status 200 "PATCH max_upload_kbps=42 → 200"
_assert_json_eq '.connection.max_upload_kbps' 42 \
	'PATCH response.connection.max_upload_kbps == 42'

# Immediate GET — no stale cache.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.max_upload_kbps' 42 \
	'IMMEDIATE GET after PATCH shows max_upload_kbps=42 (no stale cache)'

# --- 3. PATCH boolean field — bool tags need DETAIL_FULL on EC. ----
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"autoconnect":false}}' "$HOST/api/v0/preferences"
_assert_status 200 "PATCH autoconnect=false → 200"
_assert_json_eq '.connection.autoconnect' false \
	'PATCH response.connection.autoconnect == false'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.autoconnect' false \
	'IMMEDIATE GET shows autoconnect=false (EC_DETAIL_FULL honored bool)'

# Flip it back to verify the symmetric direction.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"autoconnect":true}}' "$HOST/api/v0/preferences"
_assert_json_eq '.connection.autoconnect' true \
	'PATCH autoconnect=true response shows autoconnect=true'

# --- 4. Combined PATCH — multiple fields in one body. -------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kbps":77,"autoconnect":false}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH combined (max_upload + autoconnect) → 200"
_assert_json_eq '.connection.max_upload_kbps' 77    'combined PATCH response max_upload_kbps=77'
_assert_json_eq '.connection.autoconnect'     false 'combined PATCH response autoconnect=false'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.max_upload_kbps' 77    'IMMEDIATE GET max_upload_kbps=77'
_assert_json_eq '.connection.autoconnect'     false 'IMMEDIATE GET autoconnect=false'

# --- 5. Error paths. -----------------------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH empty body → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"general":"not an object"}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH general non-object → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kbps":"forty-two"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH max_upload_kbps as string → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"tcp_port":99999}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH tcp_port out of range (>65535) → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"autoconnect":"yes"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH autoconnect as string → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH malformed JSON → 400"

# --- 5b. Extended EC categories: presence + round-trip (issue #437). -
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.directories|type)' object '/preferences has directories object'
_assert_json_eq '(.files|type)' object '/preferences has files object'
_assert_json_eq '(.servers|type)' object '/preferences has servers object'
_assert_json_eq '(.security|type)' object '/preferences has security object'
_assert_json_eq '(.message_filter|type)' object '/preferences has message_filter object'
_assert_json_eq '(.remote_controls|type)' object '/preferences has remote_controls object'
_assert_json_eq '(.online_signature|type)' object '/preferences has online_signature object'
_assert_json_eq '(.core_tweaks|type)' object '/preferences has core_tweaks object'
_assert_json_eq '(.kademlia|type)' object '/preferences has kademlia object'
_assert_json_eq '(.directories.shared|type)' array 'directories.shared is an array'
_assert_json_eq '(.files.min_free_space_mb|type)' number 'files.min_free_space_mb is numeric'
# Passwords are write-only — no password key ever appears on GET
# (user_hash is the identity hash, deliberately not matched here).
_assert_json_eq '[paths(scalars) as $p | select($p[-1]|tostring|test("password";"i"))] | length' \
	0 'no password key present in GET /preferences'
SAVED_NEW_PAUSED=$(printf '%s' "$CURL_BODY" | jq -r '.files.new_paused')
SAVED_RETRIES=$(printf '%s' "$CURL_BODY" | jq -r '.servers.dead_server_retries')

# Round-trip a bool (files) + int (servers) and confirm no stale GET.
NEW_PAUSED_TOGGLE=$([ "$SAVED_NEW_PAUSED" = "true" ] && echo false || echo true)
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"new_paused\":$NEW_PAUSED_TOGGLE},\"servers\":{\"dead_server_retries\":9}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH files+servers categories → 200"
_assert_json_eq '.files.new_paused' "$NEW_PAUSED_TOGGLE" 'files.new_paused toggled in response'
_assert_json_eq '.servers.dead_server_retries' 9 'servers.dead_server_retries=9 in response'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.files.new_paused' "$NEW_PAUSED_TOGGLE" 'files.new_paused persisted (no stale GET)'
_assert_json_eq '.servers.dead_server_retries' 9 'servers.dead_server_retries persisted'

# Wrong type on a new-category field → 400.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"files":{"min_free_space_mb":"lots"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH files.min_free_space_mb as string → 400"

# Restore the #437 fields we touched.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"new_paused\":$SAVED_NEW_PAUSED},\"servers\":{\"dead_server_retries\":$SAVED_RETRIES}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore #437 fields) → 200"

# --- 5c. Newly EC-wired prefs: media-probe + hidden-pref promotions. ---
# These eight keys were previously amulegui-local (hidden in the remote GUI
# because they were never packed into CEC_Prefs_Packet). They now round-trip
# over EC, so the REST surface must read + write them too.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.files.media_metadata_enabled|type)' boolean 'files.media_metadata_enabled is bool'
_assert_json_eq '(.files.ffprobe_path|type)' string 'files.ffprobe_path is string'
_assert_json_eq '(.files.start_next_alphabetical|type)' boolean 'files.start_next_alphabetical is bool'
_assert_json_eq '(.connection.bind_address|type)' string 'connection.bind_address is string'
_assert_json_eq '(.connection.bind_interface|type)' string 'connection.bind_interface is string'
_assert_json_eq '(.security.paranoid_filtering|type)' boolean 'security.paranoid_filtering is bool'
_assert_json_eq '(.security.use_system_ipfilter|type)' boolean 'security.use_system_ipfilter is bool'
_assert_json_eq '(.online_signature.directory|type)' string 'online_signature.directory is string'
_assert_json_eq '(.online_signature.update_frequency|type)' number 'online_signature.update_frequency is numeric'

SAVED_MM=$(printf '%s' "$CURL_BODY" | jq -r '.files.media_metadata_enabled')
SAVED_FFPROBE=$(printf '%s' "$CURL_BODY" | jq -r '.files.ffprobe_path')
SAVED_PARANOID=$(printf '%s' "$CURL_BODY" | jq -r '.security.paranoid_filtering')
SAVED_OSFREQ=$(printf '%s' "$CURL_BODY" | jq -r '.online_signature.update_frequency')
SAVED_IFACE=$(printf '%s' "$CURL_BODY" | jq -r '.connection.bind_interface')

# Round-trip a bool (files) + string (files) + bool (security) + int (onlinesig)
# + string (connection.bind_interface).
MM_TOGGLE=$([ "$SAVED_MM" = "true" ] && echo false || echo true)
PARANOID_TOGGLE=$([ "$SAVED_PARANOID" = "true" ] && echo false || echo true)
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"media_metadata_enabled\":$MM_TOGGLE,\"ffprobe_path\":\"/usr/bin/ffprobe\"},\"security\":{\"paranoid_filtering\":$PARANOID_TOGGLE},\"online_signature\":{\"update_frequency\":123},\"connection\":{\"bind_interface\":\"tun0\"}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH media-probe + security + onlinesig + iface → 200"
_assert_json_eq '.files.media_metadata_enabled' "$MM_TOGGLE" 'files.media_metadata_enabled toggled in response'
_assert_json_eq '.files.ffprobe_path' /usr/bin/ffprobe 'files.ffprobe_path set in response'
_assert_json_eq '.security.paranoid_filtering' "$PARANOID_TOGGLE" 'security.paranoid_filtering toggled in response'
_assert_json_eq '.online_signature.update_frequency' 123 'online_signature.update_frequency=123 in response'
_assert_json_eq '.connection.bind_interface' tun0 'connection.bind_interface set in response'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.files.ffprobe_path' /usr/bin/ffprobe 'files.ffprobe_path persisted (no stale GET)'
_assert_json_eq '.online_signature.update_frequency' 123 'online_signature.update_frequency persisted'
_assert_json_eq '.connection.bind_interface' tun0 'connection.bind_interface persisted'

# --- Proxy: readable fields present, round-trip, write-only password. -----
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.connection.proxy_enabled|type)' boolean 'connection.proxy_enabled is bool'
_assert_json_eq '(.connection.proxy_type|type)'    number  'connection.proxy_type is numeric'
_assert_json_eq '(.connection.proxy_host|type)'    string  'connection.proxy_host is string'
_assert_json_eq '(.connection.proxy_port|type)'    number  'connection.proxy_port is numeric'
_assert_json_eq '(.connection.proxy_auth|type)'    boolean 'connection.proxy_auth is bool'
_assert_json_eq '(.connection.proxy_user|type)'    string  'connection.proxy_user is string'
# proxy_password must NOT be present on GET (write-only).
_assert_json_eq '(.connection|has("proxy_password"))' false 'connection.proxy_password absent on GET (write-only)'

SAVED_PXEN=$(printf '%s' "$CURL_BODY" | jq -r '.connection.proxy_enabled')
SAVED_PXTYPE=$(printf '%s' "$CURL_BODY" | jq -r '.connection.proxy_type')
SAVED_PXHOST=$(printf '%s' "$CURL_BODY" | jq -r '.connection.proxy_host')
SAVED_PXPORT=$(printf '%s' "$CURL_BODY" | jq -r '.connection.proxy_port')

# Round-trip the readable fields + PATCH the write-only password in one go.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"connection":{"proxy_enabled":true,"proxy_type":2,"proxy_host":"proxy.example","proxy_port":8080,"proxy_auth":true,"proxy_user":"alice","proxy_password":"s3cret"}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH proxy (incl. write-only password) → 200"
_assert_json_eq '.connection.proxy_enabled' true 'proxy_enabled=true in response'
_assert_json_eq '.connection.proxy_type' 2 'proxy_type=2 (HTTP) in response'
_assert_json_eq '.connection.proxy_host' proxy.example 'proxy_host set in response'
_assert_json_eq '.connection.proxy_port' 8080 'proxy_port=8080 in response'
_assert_json_eq '.connection.proxy_user' alice 'proxy_user set in response'
_assert_json_eq '(.connection|has("proxy_password"))' false 'proxy_password still absent after PATCH (write-only)'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.proxy_host' proxy.example 'proxy_host persisted (no stale GET)'
_assert_json_eq '.connection.proxy_port' 8080 'proxy_port persisted'

# proxy_type out of range (>3) → 400.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"connection":{"proxy_type":9}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH proxy_type out of range (>3) → 400"

# Restore proxy readable fields (password left as-is — write-only).
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"connection\":{\"proxy_enabled\":$SAVED_PXEN,\"proxy_type\":$SAVED_PXTYPE,\"proxy_host\":\"$SAVED_PXHOST\",\"proxy_port\":$SAVED_PXPORT}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore proxy fields) → 200"

# --- P2P-router UPnP: readable, round-trip, read-only capability. --------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.connection.upnp_available|type)' boolean 'connection.upnp_available is bool'
_assert_json_eq '(.connection.upnp_enabled|type)'   boolean 'connection.upnp_enabled is bool'
_assert_json_eq '(.connection.upnp_tcp_port|type)'  number  'connection.upnp_tcp_port is numeric'
SAVED_UPNPEN=$(printf '%s' "$CURL_BODY" | jq -r '.connection.upnp_enabled')
SAVED_UPNPPORT=$(printf '%s' "$CURL_BODY" | jq -r '.connection.upnp_tcp_port')
SAVED_UPNPAVAIL=$(printf '%s' "$CURL_BODY" | jq -r '.connection.upnp_available')
UPNP_TOGGLE=$([ "$SAVED_UPNPEN" = "true" ] && echo false || echo true)
AVAIL_FLIP=$([ "$SAVED_UPNPAVAIL" = "true" ] && echo false || echo true)
# Round-trip the two settable fields, and include the read-only capability in
# the same body to prove it is ignored (response reflects the daemon, not us).
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"connection\":{\"upnp_enabled\":$UPNP_TOGGLE,\"upnp_tcp_port\":51234,\"upnp_available\":$AVAIL_FLIP}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH upnp_enabled + upnp_tcp_port (+ ignored upnp_available) → 200"
_assert_json_eq '.connection.upnp_enabled' "$UPNP_TOGGLE" 'upnp_enabled toggled in response'
_assert_json_eq '.connection.upnp_tcp_port' 51234 'upnp_tcp_port=51234 in response'
_assert_json_eq '.connection.upnp_available' "$SAVED_UPNPAVAIL" 'upnp_available unchanged (read-only, reflects daemon)'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.upnp_enabled' "$UPNP_TOGGLE" 'upnp_enabled persisted (no stale GET)'
_assert_json_eq '.connection.upnp_tcp_port' 51234 'upnp_tcp_port persisted'
# Restore.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"connection\":{\"upnp_enabled\":$SAVED_UPNPEN,\"upnp_tcp_port\":$SAVED_UPNPPORT}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore UPnP fields) → 200"

# Wrong type: a string field given a number, and a bool field given a string → 400.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"files":{"ffprobe_path":42}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH files.ffprobe_path as number → 400"
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"security":{"paranoid_filtering":"maybe"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH security.paranoid_filtering as string → 400"

# Restore the newly-wired fields we touched.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"media_metadata_enabled\":$SAVED_MM,\"ffprobe_path\":\"$SAVED_FFPROBE\"},\"security\":{\"paranoid_filtering\":$SAVED_PARANOID},\"online_signature\":{\"update_frequency\":$SAVED_OSFREQ},\"connection\":{\"bind_interface\":\"$SAVED_IFACE\"}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore newly-wired fields) → 200"

# --- 6. Restore pre-mutation state. --------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"connection\":{\"max_upload_kbps\":$SAVED_MAX_UPLOAD,\"autoconnect\":$SAVED_AUTOCONNECT}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore pre-mutation state) → 200"
_assert_json_eq '.connection.max_upload_kbps' "$SAVED_MAX_UPLOAD" \
	'restored max_upload_kbps to saved value'
_assert_json_eq '.connection.autoconnect' "$SAVED_AUTOCONNECT" \
	'restored autoconnect to saved value'

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
