#!/usr/bin/env bash
#
# amuleapi 15-preferences-patch — PATCH /preferences.
#
# Endpoint:
#   PATCH /api/v0/preferences
#       body: { general?, connection?, directories?, files?, servers?,
#               security?, message_filter?, remote_controls?,
#               online_signature?, advanced?, kad? }  (issue #437)
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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 15-preferences-patch smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# Save the pre-mutation state so we can restore everything at the
# end. We only modify two fields (max_upload_kibibytes_per_second + autoconnect) so
# the operator's daemon doesn't end the smoke in an unexpected state.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
SAVED_MAX_UPLOAD=$(printf '%s' "$CURL_BODY" | jq -r '.connection.max_upload_kibibytes_per_second')
SAVED_AUTOCONNECT=$(printf '%s' "$CURL_BODY" | jq -r '.connection.autoconnect')
echo "    info: saved state max_upload_kibibytes_per_second=$SAVED_MAX_UPLOAD autoconnect=$SAVED_AUTOCONNECT"

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kibibytes_per_second":42}}' "$HOST/api/v0/preferences"
_assert_status 401 "PATCH /preferences (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X PATCH -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"connection":{"max_upload_kibibytes_per_second":42}}' "$HOST/api/v0/preferences"
	_assert_status 403 "PATCH /preferences (guest) → 403"
else
	echo "    info: no guest pass; admin-gate skipped"
fi

# --- 2. PATCH numeric field — response + no-stale GET. -------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kibibytes_per_second":42}}' "$HOST/api/v0/preferences"
_assert_status 200 "PATCH max_upload_kibibytes_per_second=42 → 200"
_assert_json_eq '.connection.max_upload_kibibytes_per_second' 42 \
	'PATCH response.connection.max_upload_kibibytes_per_second == 42'

# Immediate GET — no stale cache.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.max_upload_kibibytes_per_second' 42 \
	'IMMEDIATE GET after PATCH shows max_upload_kibibytes_per_second=42 (no stale cache)'

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
	-d '{"connection":{"max_upload_kibibytes_per_second":77,"autoconnect":false}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH combined (max_upload + autoconnect) → 200"
_assert_json_eq '.connection.max_upload_kibibytes_per_second' 77    'combined PATCH response max_upload_kibibytes_per_second=77'
_assert_json_eq '.connection.autoconnect'     false 'combined PATCH response autoconnect=false'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.max_upload_kibibytes_per_second' 77    'IMMEDIATE GET max_upload_kibibytes_per_second=77'
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
	-d '{"connection":{"max_upload_kibibytes_per_second":"forty-two"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH max_upload_kibibytes_per_second as string → 400"

# Saved because the 65532 probe below is a real write: leaving the daemon's
# ed2k port on the ceiling would outlive the script.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
SAVED_TCPPORT=$(printf '%s' "$CURL_BODY" | jq -r '.connection.tcp_port')

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"tcp_port":99999}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH tcp_port out of range (>65532) → 400"

# 65533..65535 parse as a port but the core cannot use them: SetPort()
# substitutes DEFAULT_TCP_PORT when val + 3 exceeds 65535, because the server
# UDP socket is TCP+3. A 200 here meant the daemon listened on 4662 instead
# (#1174).
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"tcp_port":65534}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH tcp_port=65534 → 400 (TCP+3 would overflow)"
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"tcp_port":65532}}' "$HOST/api/v0/preferences"
_assert_status 200 "PATCH tcp_port=65532 → 200 (the real ceiling)"
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.tcp_port' 65532 "tcp_port=65532 reads back unchanged"

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
_assert_json_eq '(.advanced|type)' object '/preferences has advanced object'
_assert_json_eq '(.kad|type)' object '/preferences has kad object'
_assert_json_eq '(.directories.shared_paths|type)' array 'directories.shared_paths is an array'
_assert_json_eq '(.files.min_free_space_mebibytes|type)' number 'files.min_free_space_mebibytes is numeric'
# Passwords are write-only — no password key ever appears on GET
# (user_hash is the identity hash, deliberately not matched here).
_assert_json_eq '[paths(scalars) as $p | select($p[-1]|tostring|test("password";"i"))] | length' \
	0 'no password key present in GET /preferences'
SAVED_NEW_PAUSED=$(printf '%s' "$CURL_BODY" | jq -r '.files.add_new_downloads_paused')
SAVED_RETRIES=$(printf '%s' "$CURL_BODY" | jq -r '.servers.dead_server_retry_count')

# Round-trip a bool (files) + int (servers) and confirm no stale GET.
NEW_PAUSED_TOGGLE=$([ "$SAVED_NEW_PAUSED" = "true" ] && echo false || echo true)
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"add_new_downloads_paused\":$NEW_PAUSED_TOGGLE},\"servers\":{\"dead_server_retry_count\":9}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH files+servers categories → 200"
_assert_json_eq '.files.add_new_downloads_paused' "$NEW_PAUSED_TOGGLE" 'files.add_new_downloads_paused toggled in response'
_assert_json_eq '.servers.dead_server_retry_count' 9 'servers.dead_server_retry_count=9 in response'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.files.add_new_downloads_paused' "$NEW_PAUSED_TOGGLE" 'files.add_new_downloads_paused persisted (no stale GET)'
_assert_json_eq '.servers.dead_server_retry_count' 9 'servers.dead_server_retry_count persisted'

# Wrong type on a new-category field → 400.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"files":{"min_free_space_mebibytes":"lots"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH files.min_free_space_mebibytes as string → 400"

# Restore the #437 fields we touched.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"add_new_downloads_paused\":$SAVED_NEW_PAUSED},\"servers\":{\"dead_server_retry_count\":$SAVED_RETRIES}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore #437 fields) → 200"

# --- 5c. Newly EC-wired prefs: media-probe + hidden-pref promotions. ---
# These eight keys were previously amulegui-local (hidden in the remote GUI
# because they were never packed into CEC_Prefs_Packet). They now round-trip
# over EC, so the REST surface must read + write them too.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.files.media_metadata_enabled|type)' boolean 'files.media_metadata_enabled is bool'
_assert_json_eq '(.files.ffprobe_path|type)' string 'files.ffprobe_path is string'
_assert_json_eq '(.files.on_finished_start_next_alphabetically|type)' boolean 'files.on_finished_start_next_alphabetically is bool'
_assert_json_eq '(.connection.bind_address|type)' string 'connection.bind_address is string'
_assert_json_eq '(.connection.bind_interface|type)' string 'connection.bind_interface is string'
_assert_json_eq '(.security.reject_spoofed_source_ips|type)' boolean 'security.reject_spoofed_source_ips is bool'
_assert_json_eq '(.security.system_ipfilter_enabled|type)' boolean 'security.system_ipfilter_enabled is bool'
_assert_json_eq '(.online_signature.directory|type)' string 'online_signature.directory is string'
_assert_json_eq '(.online_signature.update_frequency_seconds|type)' number 'online_signature.update_frequency_seconds is numeric'

SAVED_MM=$(printf '%s' "$CURL_BODY" | jq -r '.files.media_metadata_enabled')
SAVED_FFPROBE=$(printf '%s' "$CURL_BODY" | jq -r '.files.ffprobe_path')
SAVED_PARANOID=$(printf '%s' "$CURL_BODY" | jq -r '.security.reject_spoofed_source_ips')
SAVED_OSFREQ=$(printf '%s' "$CURL_BODY" | jq -r '.online_signature.update_frequency_seconds')
SAVED_IFACE=$(printf '%s' "$CURL_BODY" | jq -r '.connection.bind_interface')
SAVED_KADREASK=$(printf '%s' "$CURL_BODY" | jq -r '.advanced.kad_source_reask_minutes')
SAVED_SRCREASK=$(printf '%s' "$CURL_BODY" | jq -r '.advanced.source_reask_minutes')

# Round-trip a bool (files) + string (files) + bool (security) + int (onlinesig)
# + string (connection.bind_interface).
MM_TOGGLE=$([ "$SAVED_MM" = "true" ] && echo false || echo true)
PARANOID_TOGGLE=$([ "$SAVED_PARANOID" = "true" ] && echo false || echo true)
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"media_metadata_enabled\":$MM_TOGGLE,\"ffprobe_path\":\"/usr/bin/ffprobe\"},\"security\":{\"reject_spoofed_source_ips\":$PARANOID_TOGGLE},\"online_signature\":{\"update_frequency_seconds\":123},\"connection\":{\"bind_interface\":\"tun0\"}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH media-probe + security + onlinesig + iface → 200"
_assert_json_eq '.files.media_metadata_enabled' "$MM_TOGGLE" 'files.media_metadata_enabled toggled in response'
_assert_json_eq '.files.ffprobe_path' /usr/bin/ffprobe 'files.ffprobe_path set in response'
_assert_json_eq '.security.reject_spoofed_source_ips' "$PARANOID_TOGGLE" 'security.reject_spoofed_source_ips toggled in response'
_assert_json_eq '.online_signature.update_frequency_seconds' 123 'online_signature.update_frequency_seconds=123 in response'
_assert_json_eq '.connection.bind_interface' tun0 'connection.bind_interface set in response'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.files.ffprobe_path' /usr/bin/ffprobe 'files.ffprobe_path persisted (no stale GET)'
_assert_json_eq '.online_signature.update_frequency_seconds' 123 'online_signature.update_frequency_seconds persisted'
_assert_json_eq '.connection.bind_interface' tun0 'connection.bind_interface persisted'

# --- 5c-bis. message_filter show-in-log + comment filter (#596). ----------
# Newly EC-wired: previously amulegui-local / unreachable over EC.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.message_filter.log_filtered_messages|type)' boolean 'message_filter.log_filtered_messages is bool'
_assert_json_eq '(.message_filter.filter_comments|type)' boolean 'message_filter.filter_comments is bool'
_assert_json_eq '(.message_filter.comment_keywords|type)' string 'message_filter.comment_keywords is string'
SAVED_SHOW_IN_LOG=$(printf '%s' "$CURL_BODY" | jq -r '.message_filter.log_filtered_messages')
SAVED_FILTER_COMMENTS=$(printf '%s' "$CURL_BODY" | jq -r '.message_filter.filter_comments')
SAVED_COMMENT_KW=$(printf '%s' "$CURL_BODY" | jq -r '.message_filter.comment_keywords')
SHOW_TOGGLE=$([ "$SAVED_SHOW_IN_LOG" = "true" ] && echo false || echo true)
FC_TOGGLE=$([ "$SAVED_FILTER_COMMENTS" = "true" ] && echo false || echo true)
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"message_filter\":{\"log_filtered_messages\":$SHOW_TOGGLE,\"filter_comments\":$FC_TOGGLE,\"comment_keywords\":\"spam,ads\"}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH message_filter (log_filtered_messages+filter_comments+comment_keywords) → 200"
_assert_json_eq '.message_filter.log_filtered_messages' "$SHOW_TOGGLE" 'message_filter.log_filtered_messages toggled in response'
_assert_json_eq '.message_filter.filter_comments' "$FC_TOGGLE" 'message_filter.filter_comments toggled in response'
_assert_json_eq '.message_filter.comment_keywords' 'spam,ads' 'message_filter.comment_keywords set in response'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.message_filter.comment_keywords' 'spam,ads' 'message_filter.comment_keywords persisted (no stale GET)'
# Restore.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"message_filter\":{\"log_filtered_messages\":$SAVED_SHOW_IN_LOG,\"filter_comments\":$SAVED_FILTER_COMMENTS,\"comment_keywords\":\"$SAVED_COMMENT_KW\"}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore message_filter fields) → 200"

# --- 5d. mmap (#565): capability flag + capability-gated round-trip. ------
# files.mmap_supported is a read-only daemon capability; files.mmap_enabled is
# only settable when it is true. This branch adapts to whichever daemon runs
# the smoke: a mmap-capable core exercises the round-trip, a non-mmap core
# (e.g. Windows, -DENABLE_MMAP=OFF) exercises the 409 capability gate.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.files.mmap_supported|type)' boolean 'files.mmap_supported is bool (read-only capability)'
_assert_json_eq '(.files.mmap_enabled|type)' boolean 'files.mmap_enabled is bool'
MMAP_SUPPORTED=$(printf '%s' "$CURL_BODY" | jq -r '.files.mmap_supported')
SAVED_MMAP=$(printf '%s' "$CURL_BODY" | jq -r '.files.mmap_enabled')
if [ "$MMAP_SUPPORTED" = "true" ]; then
	MMAP_TOGGLE=$([ "$SAVED_MMAP" = "true" ] && echo false || echo true)
	# Send the read-only mmap_supported alongside a real toggle: it must be
	# ignored (not rejected), and GET must still report support = true.
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
		-d "{\"files\":{\"mmap_enabled\":$MMAP_TOGGLE,\"mmap_supported\":false}}" \
		"$HOST/api/v0/preferences"
	_assert_status 200 "PATCH files.mmap_enabled (daemon supports mmap) → 200"
	_assert_json_eq '.files.mmap_enabled' "$MMAP_TOGGLE" 'files.mmap_enabled toggled in response'
	_assert_json_eq '.files.mmap_supported' true 'files.mmap_supported read-only (ignored on PATCH)'
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
	_assert_json_eq '.files.mmap_enabled' "$MMAP_TOGGLE" 'files.mmap_enabled persisted (no stale GET)'
	# Restore.
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
		-d "{\"files\":{\"mmap_enabled\":$SAVED_MMAP}}" "$HOST/api/v0/preferences" >/dev/null 2>&1
else
	echo "    info: daemon built without mmap — exercising the 409 capability gate"
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
		-d '{"files":{"mmap_enabled":true}}' "$HOST/api/v0/preferences"
	_assert_status 409 "PATCH files.mmap_enabled on non-mmap daemon → 409"
	_assert_json_eq '.error.code' option_not_supported \
		'the 409 names option_not_supported, not a bare conflict'
fi

# --- Proxy: readable fields present, round-trip, write-only password. -----
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.connection.proxy_enabled|type)' boolean 'connection.proxy_enabled is bool'
_assert_json_eq '(.connection.proxy_type|type)'    string  'connection.proxy_type is an enum string (#655)'
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
	-d '{"connection":{"proxy_enabled":true,"proxy_type":"http","proxy_host":"proxy.example","proxy_port":8080,"proxy_auth":true,"proxy_user":"alice","proxy_password":"s3cret"}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH proxy (incl. write-only password) → 200"
_assert_json_eq '.connection.proxy_enabled' true 'proxy_enabled=true in response'
_assert_json_eq '.connection.proxy_type' http 'proxy_type="http" in response'
_assert_json_eq '.connection.proxy_host' proxy.example 'proxy_host set in response'
_assert_json_eq '.connection.proxy_port' 8080 'proxy_port=8080 in response'
_assert_json_eq '.connection.proxy_user' alice 'proxy_user set in response'
_assert_json_eq '(.connection|has("proxy_password"))' false 'proxy_password still absent after PATCH (write-only)'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.proxy_host' proxy.example 'proxy_host persisted (no stale GET)'
_assert_json_eq '.connection.proxy_port' 8080 'proxy_port persisted'

# proxy_type outside the enum → 400, and the pre-#655 wire int is no
# longer accepted either.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"connection":{"proxy_type":"telepathy"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH proxy_type unknown enum value → 400"
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"connection":{"proxy_type":2}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH proxy_type as the old wire int → 400 (#655)"

# Restore proxy readable fields (password left as-is — write-only).
# proxy_type is omitted when it came back empty: that is CProxyType
# PROXY_NONE (-1), which has no enum string and is not settable (#655).
if [ -n "$SAVED_PXTYPE" ]; then
	RESTORE_PXTYPE="\"proxy_type\":\"$SAVED_PXTYPE\","
else
	RESTORE_PXTYPE=""
fi
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"connection\":{\"proxy_enabled\":$SAVED_PXEN,$RESTORE_PXTYPE\"proxy_host\":\"$SAVED_PXHOST\",\"proxy_port\":$SAVED_PXPORT}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore proxy fields) → 200"

# --- Nested remote_controls (#655): round-trip through the sub-objects. ---
# Both subsystems pack into one EC category, so this also proves the two
# sub-objects can be sent together without one clobbering the other.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
SAVED_WS_PORT=$(printf '%s' "$CURL_BODY" | jq -r '.remote_controls.webserver.port')
SAVED_WS_REFRESH=$(printf '%s' "$CURL_BODY" | jq -r '.remote_controls.webserver.refresh_seconds')
SAVED_API_BIND=$(printf '%s' "$CURL_BODY" | jq -r '.remote_controls.amuleapi.bind_address')

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"remote_controls":{"webserver":{"port":4711,"refresh_seconds":123},"amuleapi":{"bind_address":"127.0.0.1"}}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH nested remote_controls (webserver + amuleapi) → 200"
_assert_json_eq '.remote_controls.webserver.port' 4711 'webserver.port=4711 in response'
_assert_json_eq '.remote_controls.webserver.refresh_seconds' 123 'webserver.refresh_seconds=123 in response'
_assert_json_eq '.remote_controls.amuleapi.bind_address' 127.0.0.1 'amuleapi.bind_address set in response'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.remote_controls.webserver.port' 4711 'webserver.port persisted (no stale GET)'
_assert_json_eq '.remote_controls.amuleapi.bind_address' 127.0.0.1 'amuleapi.bind_address persisted'

# The flat pre-#655 keys are no longer a write path — they are simply
# unknown fields now, so a body carrying only those changes nothing.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"remote_controls":{"webserver_port":9999}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH flat remote_controls.webserver_port → 400 (no known fields, #655)"

# A sub-object that is not an object is rejected, like the categories are.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"remote_controls":{"webserver":"nope"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH remote_controls.webserver non-object → 400"

# amuleapi's own passwords stay owned by PATCH /auth/passwords, now under
# the nested key.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"remote_controls":{"amuleapi":{"password":"nope"}}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH remote_controls.amuleapi.password → 400 (managed via /auth/passwords)"

# Restore.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"remote_controls\":{\"webserver\":{\"port\":$SAVED_WS_PORT,\"refresh_seconds\":$SAVED_WS_REFRESH},\"amuleapi\":{\"bind_address\":\"$SAVED_API_BIND\"}}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore remote_controls fields) → 200"

# --- P2P-router UPnP: readable, round-trip, read-only capability. --------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '(.connection.upnp_supported|type)' boolean 'connection.upnp_supported is bool'
_assert_json_eq '(.connection.upnp_enabled|type)'   boolean 'connection.upnp_enabled is bool'
_assert_json_eq '(.connection.upnp_control_point_port|type)'  number  'connection.upnp_control_point_port is numeric'
SAVED_UPNPEN=$(printf '%s' "$CURL_BODY" | jq -r '.connection.upnp_enabled')
SAVED_UPNPPORT=$(printf '%s' "$CURL_BODY" | jq -r '.connection.upnp_control_point_port')
SAVED_UPNPAVAIL=$(printf '%s' "$CURL_BODY" | jq -r '.connection.upnp_supported')
UPNP_TOGGLE=$([ "$SAVED_UPNPEN" = "true" ] && echo false || echo true)
AVAIL_FLIP=$([ "$SAVED_UPNPAVAIL" = "true" ] && echo false || echo true)
# Round-trip the two settable fields, and include the read-only capability in
# the same body to prove it is ignored (response reflects the daemon, not us).
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"connection\":{\"upnp_enabled\":$UPNP_TOGGLE,\"upnp_control_point_port\":51234,\"upnp_supported\":$AVAIL_FLIP}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH upnp_enabled + upnp_control_point_port (+ ignored upnp_supported) → 200"
_assert_json_eq '.connection.upnp_enabled' "$UPNP_TOGGLE" 'upnp_enabled toggled in response'
_assert_json_eq '.connection.upnp_control_point_port' 51234 'upnp_control_point_port=51234 in response'
_assert_json_eq '.connection.upnp_supported' "$SAVED_UPNPAVAIL" 'upnp_supported unchanged (read-only, reflects daemon)'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.upnp_enabled' "$UPNP_TOGGLE" 'upnp_enabled persisted (no stale GET)'
_assert_json_eq '.connection.upnp_control_point_port' 51234 'upnp_control_point_port persisted'
# Restore.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"connection\":{\"upnp_enabled\":$SAVED_UPNPEN,\"upnp_control_point_port\":$SAVED_UPNPPORT}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore UPnP fields) → 200"

# Wrong type: a string field given a number, and a bool field given a string → 400.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"files":{"ffprobe_path":42}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH files.ffprobe_path as number → 400"
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"security":{"reject_spoofed_source_ips":"maybe"}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH security.reject_spoofed_source_ips as string → 400"

# Restore the newly-wired fields we touched.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"files\":{\"media_metadata_enabled\":$SAVED_MM,\"ffprobe_path\":\"$SAVED_FFPROBE\"},\"security\":{\"reject_spoofed_source_ips\":$SAVED_PARANOID},\"online_signature\":{\"update_frequency_seconds\":$SAVED_OSFREQ},\"connection\":{\"bind_interface\":\"$SAVED_IFACE\"}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore newly-wired fields) → 200"

# --- 6. Restore pre-mutation state. --------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"connection\":{\"max_upload_kibibytes_per_second\":$SAVED_MAX_UPLOAD,\"autoconnect\":$SAVED_AUTOCONNECT}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore pre-mutation state) → 200"
_assert_json_eq '.connection.max_upload_kibibytes_per_second' "$SAVED_MAX_UPLOAD" \
	'restored max_upload_kibibytes_per_second to saved value'
_assert_json_eq '.connection.autoconnect' "$SAVED_AUTOCONNECT" \
	'restored autoconnect to saved value'

# --- advanced intervals round-trip exactly (#1159 section 5). ---------
#
# The core stores whole minutes and its accessors multiply by 60000, so EC
# carries milliseconds that are always a multiple of 60000. The API used to
# expose that raw: a client writing 90000 read back 60000, and one writing
# 30000 read back 0 -- accepted, reported as success, changed underneath. The
# fields speak minutes now, so what goes in comes back.
# The probe values sit INSIDE each field's supported range and are not the
# range's own endpoints, so they prove the value is carried rather than snapped
# to a bound: LoadAllItems() clamps kad_reask to 30..60 and source_reask to
# 15..60 at the next daemon start, and 7 -- what this loop used to send -- is
# below both (#1174).
for FIELD_PROBE in "kad_source_reask_minutes 31" "source_reask_minutes 16"; do
	set -- $FIELD_PROBE
	FIELD=$1
	VALUE=$2
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"advanced\":{\"$FIELD\":$VALUE}}" "$HOST/api/v0/preferences"
	_assert_status 200 "PATCH advanced.$FIELD=$VALUE -> 200"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
	_assert_json_eq ".advanced.$FIELD" "$VALUE" "advanced.$FIELD reads back what was written"
done

# One minute used to be a 200 that the daemon threw away at the next start --
# LoadAllItems() clamps this field up to 30. The old assertion here checked the
# right property (a small value is not truncated to 0 by the ms->minutes
# conversion) with a probe outside the core's range, so it passed on a value
# the daemon would not keep. Below the floor is a 400 now (#1174).
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"advanced":{"kad_source_reask_minutes":1}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH advanced.kad_source_reask_minutes=1 -> 400 (below the 30 floor)"
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.advanced.kad_source_reask_minutes' 31 "the rejected write left the previous value alone"

# --- online_signature.update_frequency_seconds is bounded (#1159 section 4).
#
# The schema declared the full uint32 range while CPreferences::SetOSUpdate
# takes a uint16, so anything above 65535 wrapped on the way in: 86400 (daily)
# became 20864, the PATCH reported success, and the following GET showed the
# rewritten value. A rejected write is recoverable; a silently rewritten one is
# not, because the client has no way to know it happened.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"online_signature":{"update_frequency_seconds":86400}}' \
	"$HOST/api/v0/preferences"
_assert_status 400 "PATCH online_signature.update_frequency_seconds=86400 -> 400 (would wrap to 20864)"

# The boundary itself is still accepted.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"online_signature":{"update_frequency_seconds":65535}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH online_signature.update_frequency_seconds=65535 -> 200"

# Saved before the boundary sweep below, which leaves every field it touches on
# an endpoint. Restored with the rest at the end of the file.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
SAVED_FILEBUF=$(printf '%s' "$CURL_BODY" | jq -r '.advanced.file_buffer_bytes')
SAVED_ULQUEUE=$(printf '%s' "$CURL_BODY" | jq -r '.advanced.max_upload_queue_client_count')
SAVED_CONN5=$(printf '%s' "$CURL_BODY" | jq -r '.advanced.max_new_connections_per_5_seconds')
SAVED_KADSEARCH=$(printf '%s' "$CURL_BODY" | jq -r '.advanced.kad_max_concurrent_source_search_count')
SAVED_MAXUL=$(printf '%s' "$CURL_BODY" | jq -r '.connection.max_upload_kibibytes_per_second')
SAVED_MAXDL=$(printf '%s' "$CURL_BODY" | jq -r '.connection.max_download_kibibytes_per_second')

# --- #1174: every numeric domain that is narrower than its type. ---
#
# Each row was measured against a live daemon before the bounds landed: the
# write was a 200 and the value came back changed, by integer division, by a
# uint8/uint16 wrap, or by a clamp that does not run until the next start.
# Appended at the end of the file on purpose -- the sections here share one
# daemon, and these leave preferences at their boundary values.
#
# reject: the value, and what the daemon used to turn it into.
for CASE in \
	"advanced file_buffer_bytes 4000000 150000_uint8_wrap" \
	"advanced file_buffer_bytes 14999 0_divided_away" \
	"advanced file_buffer_bytes 20000 15000_truncated_to_the_step" \
	"advanced max_upload_queue_client_count 30000 4400_uint8_wrap" \
	"advanced max_upload_queue_client_count 250 200_truncated_to_the_step" \
	"advanced max_new_connections_per_5_seconds 70000 4464_uint16_wrap" \
	"advanced kad_max_concurrent_source_search_count 100000 34464_uint16_wrap" \
	"advanced kad_max_concurrent_source_search_count 1 5_clamped_at_next_start" \
	"advanced source_reask_minutes 7 15_clamped_at_next_start" \
	; do
	set -- $CASE
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"$1\":{\"$2\":$3}}" "$HOST/api/v0/preferences"
	_assert_status 400 "PATCH $1.$2=$3 -> 400 (was silently $4)"
done

# The 400 body names which bound was hit and by what unit -- "out of range"
# alone leaves a caller guessing which end and, for a step, why an in-range
# value was refused. Assert the message for one range case and one step case.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"tcp_port":99999}}' "$HOST/api/v0/preferences"
_assert_json_eq '.error.message | contains("(1-65532)")' true \
	"tcp_port 400 names the range in the message"
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"advanced":{"file_buffer_bytes":20000}}' "$HOST/api/v0/preferences"
_assert_json_eq '.error.message | contains("multiple of 15000")' true \
	"file_buffer_bytes 400 names the step in the message"

# accept: the domain's own endpoints, which must round-trip untouched. A bound
# that rejects its own boundary is the failure mode this half guards.
for CASE in \
	"advanced file_buffer_bytes 3825000" \
	"advanced file_buffer_bytes 15000" \
	"advanced file_buffer_bytes 0" \
	"advanced max_upload_queue_client_count 25500" \
	"advanced max_upload_queue_client_count 100" \
	"advanced max_new_connections_per_5_seconds 65535" \
	"advanced kad_max_concurrent_source_search_count 5" \
	"advanced kad_max_concurrent_source_search_count 50" \
	"advanced source_reask_minutes 15" \
	"advanced source_reask_minutes 60" \
	"advanced kad_source_reask_minutes 30" \
	"advanced kad_source_reask_minutes 60" \
	; do
	set -- $CASE
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"$1\":{\"$2\":$3}}" "$HOST/api/v0/preferences"
	_assert_status 200 "PATCH $1.$2=$3 -> 200 (domain boundary)"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
	_assert_json_eq ".$1.$2" "$3" "$1.$2=$3 reads back unchanged"
done

# The cross-field rewrite is documented rather than rejected: a low upload cap
# forces the download cap to 3x (below 4 kB/s) or 4x (below 10). Asserted so
# the documented behaviour has a test, and so a future change to CheckUlDlRatio
# cannot alter it silently.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"connection":{"max_upload_kibibytes_per_second":3,"max_download_kibibytes_per_second":100}}' \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH max_upload_kibibytes_per_second=3 -> 200"
_assert_json_eq '.connection.max_download_kibibytes_per_second' 9 \
	"a sub-4 kB/s upload cap forces max_download_kibibytes_per_second to 3x, echoed in the PATCH reply"
# --- Restore what the two #1159 probes above changed. --------------
#
# Section 6 restores before those probes run, so the last writes of the script
# used to be its own probe values -- 7 and 1 minutes and a 65535-second
# online-signature interval, left on whatever daemon the suite was pointed at.
# This has to stay the last mutation in the file.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"advanced\":{\"kad_source_reask_minutes\":$SAVED_KADREASK,\"source_reask_minutes\":$SAVED_SRCREASK,\"file_buffer_bytes\":$SAVED_FILEBUF,\"max_upload_queue_client_count\":$SAVED_ULQUEUE,\"max_new_connections_per_5_seconds\":$SAVED_CONN5,\"kad_max_concurrent_source_search_count\":$SAVED_KADSEARCH},\"connection\":{\"tcp_port\":$SAVED_TCPPORT,\"max_upload_kibibytes_per_second\":$SAVED_MAXUL,\"max_download_kibibytes_per_second\":$SAVED_MAXDL},\"online_signature\":{\"update_frequency_seconds\":$SAVED_OSFREQ}}" \
	"$HOST/api/v0/preferences"
_assert_status 200 "PATCH (restore advanced + connection + onlinesig) -> 200"
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.connection.tcp_port' "$SAVED_TCPPORT" \
	'restored connection.tcp_port to the saved value'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/preferences"
_assert_json_eq '.advanced.kad_source_reask_minutes' "$SAVED_KADREASK" \
	'restored advanced.kad_source_reask_minutes to the saved value'
_assert_json_eq '.advanced.source_reask_minutes' "$SAVED_SRCREASK" \
	'restored advanced.source_reask_minutes to the saved value'
_assert_json_eq '.online_signature.update_frequency_seconds' "$SAVED_OSFREQ" \
	'restored online_signature.update_frequency_seconds to the saved value'

# --- 7. `geoip.update_now` moved out to POST /geoip/update (#1189). --
#
# It was a write-only boolean in this payload; it is an action, so it is a
# route now. Both halves of that move are contract, so both are asserted:
# the old key is refused with a message naming the endpoint, and the endpoint
# exists with the right method and auth.
#
# The 202 happy path is deliberately NOT exercised: it makes the daemon fetch
# a real database from db-ip.com, and a smoke suite should not hit a third
# party on every run. Everything up to the fetch is covered here.
#
# Placed after the restore block because none of it mutates anything -- the
# restore has to stay the last *mutation* in this file.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"geoip":{"update_now":true}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH geoip.update_now -> 400 (moved to POST /geoip/update)"
_assert_json_eq '.error.code' bad_request \
	'the refusal carries error.code=bad_request'
_assert_json_eq '(.error.message | test("POST /geoip/update"))' true \
	'the refusal names the endpoint that took the action over'

_curl "$HOST/api/v0/geoip/update"
_assert_status 405 "GET /geoip/update -> 405 (POST only)"
_assert_json_eq '.error.code' method_not_allowed \
	'/geoip/update GET 405 carries error.code=method_not_allowed'

_curl -X POST "$HOST/api/v0/geoip/update"
_assert_status 401 "POST /geoip/update without auth -> 401"

# GUEST is not ADMIN: the fetch writes daemon state, so it is admin-only like
# its three sibling update routes.
if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/geoip/update"
	_assert_status 403 "POST /geoip/update as guest -> 403"
else
	echo "    info: no guest pass; /geoip/update admin-gate skipped"
fi

# --- upload_slot_min_kibibytes_per_second has a floor of 1. --------
# The core's setter clamps anything lower up to 1, so without a declared
# minimum a 0 answered 200 and read back as 1 -- exactly the silent
# daemon-side rewrite the schema bounds exist to turn into a 400.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"connection":{"upload_slot_min_kibibytes_per_second":0}}' "$HOST/api/v0/preferences"
_assert_status 400 "PATCH upload_slot_min_kibibytes_per_second=0 -> 400 (below the floor)"
_assert_json_eq '.error.code' bad_request \
	'the below-floor slot value carries error.code=bad_request'

# 1 is the floor itself, so it must still be accepted.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"connection":{"upload_slot_min_kibibytes_per_second":1}}' "$HOST/api/v0/preferences"
_assert_status 200 "PATCH upload_slot_min_kibibytes_per_second=1 -> 200 (the floor is inclusive)"
_assert_json_eq '.connection.upload_slot_min_kibibytes_per_second' 1 \
	'the floor value round-trips unchanged'

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
