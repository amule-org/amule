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
