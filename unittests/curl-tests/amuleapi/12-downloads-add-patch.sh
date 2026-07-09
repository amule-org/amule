#!/usr/bin/env bash
#
# amuleapi 12-downloads-add-patch — download lifecycle mutations.
#
# Endpoints landed:
#   POST  /api/v0/downloads             — add a download by ed2k_link
#   PATCH /api/v0/downloads/{hash}      — status/priority/category
#
# Mutate-then-refresh contract: every mutation handler runs
# RefresherTick inline after the EC roundtrip succeeds, so the
# response body AND the IMMEDIATE-next GET both reflect post-mutation
# state. No sleep loops, no stale cache window. This smoke pins that
# invariant — every PATCH is followed by a no-sleep GET that must
# show the same state the PATCH response showed.
#
# The smoke uses the Ubuntu 24.04.4 ISO ed2k link as its test artifact
# (provided by the operator). Hash:
# 0031C9CBA65C50DD2015C184B2CA2C88. The ISO is a stable, well-seeded
# ed2k file; amuled adds it to m_filelist within ~1 refresher tick.
# 5b's smoke cleans up via DELETE.
#
# Role gate: mutations require ADMIN. Guest tokens get 403 forbidden.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

# Stable test artifact: Ubuntu 24.04.4 desktop ISO. Lowercase hash as
# stored on the API side (the wire path is case-insensitive but the
# State cache is keyed in lowercase).
TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_12_downloads_add_patch_body.XXXXXX)
trap '
	rm -f "$CURL_BODY_FILE"
	# Best-effort partfile cleanup so the Ubuntu ISO doesn'\''t survive
	# a failed run and pin disk on the Windows VM (per
	# feedback_clean_temp_partfiles_after_test).
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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 12-downloads-add-patch smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] \
	|| _die "admin login failed (need --set-admin-pass=$ADMIN_PASS on the daemon)"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
if [ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ]; then
	HAVE_GUEST=1
fi

sleep 4

# --- 1. Auth gate (no token → 401). --------------------------------
_curl -X POST -H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads"
_assert_status 401 "POST /downloads (no token) → 401"

_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"status":"paused"}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 401 "PATCH /downloads/{hash} (no token) → 401"

# --- 2. Admin gate (guest → 403). ----------------------------------
if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads"
	_assert_status 403 "POST /downloads (guest token) → 403"
	_assert_json_eq '.error.code' forbidden \
		'POST /downloads guest carries error.code=forbidden'

	_curl -X PATCH -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"status":"paused"}' "$HOST/api/v0/downloads/$TEST_HASH"
	_assert_status 403 "PATCH /downloads/{hash} (guest token) → 403"
else
	echo "    info: no guest password set on daemon; admin-gate test skipped"
fi

# --- 3. POST /downloads happy: add the test ISO. -------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"ed2k_link\":\"$TEST_LINK\"}" "$HOST/api/v0/downloads"
_assert_status 202 "POST /downloads (Ubuntu ISO) → 202"
# Unified per-item envelope (#358): one accepted result keyed by the link.
_assert_json_eq '.results | length' 1 'POST /downloads returns one result'
_assert_json_eq '.results[0].ok' true 'POST /downloads results[0].ok==true'
_assert_json_eq ".results[0].id" "$TEST_LINK" 'POST /downloads results[0].id echoes the link'

# Poll until the new partfile surfaces in /downloads (amuled's ADD_LINK
# is async — allocation + hashing happen post-roundtrip). Bound at
# ~5 s with 200 ms steps. Production refresher catches it in ≤1 tick.
APPEARED=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/downloads?include_completed=1"
	if printf '%s' "$CURL_BODY" \
	   | jq -e --arg h "$TEST_HASH" '.downloads[] | select(.hash == $h)' \
	   >/dev/null 2>&1; then
		APPEARED=1
		break
	fi
	sleep 0.2
done
if [ "$APPEARED" = "1" ]; then
	_pass "Ubuntu ISO surfaced in /downloads via async ADD_LINK"
else
	_die "Ubuntu ISO never surfaced; ADD_LINK semantics may have shifted"
fi

# --- 4. POST /downloads error paths. -------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"ed2k_link":"http://not-an-ed2k.com/foo"}' "$HOST/api/v0/downloads"
_assert_status 400 "POST /downloads (non-ed2k URL) → 400"
_assert_json_eq '.error.code' bad_request \
	'POST /downloads invalid URL carries error.code=bad_request'

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/downloads"
_assert_status 400 "POST /downloads (missing ed2k_link) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d 'not json' "$HOST/api/v0/downloads"
_assert_status 400 "POST /downloads (malformed JSON) → 400"

# --- 5. PATCH happy paths + no-stale-cache invariant. --------------
#
# Save the pre-mutation state so we can restore it at the end.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
SAVED_STATUS=$(printf '%s' "$CURL_BODY" | jq -r '.status')
SAVED_PRIORITY=$(printf '%s' "$CURL_BODY" | jq -r '.priority')
SAVED_CATEGORY=$(printf '%s' "$CURL_BODY" | jq -r '.category')
echo "    info: saved state status=$SAVED_STATUS priority=$SAVED_PRIORITY category=$SAVED_CATEGORY"

# 5a. PATCH status=paused. Response body must show paused. Immediate
# GET (no sleep) must also show paused — pins the mutate-then-refresh
# contract.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"status":"paused"}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "PATCH /downloads/{hash} status=paused → 200"
_assert_json_eq '.status' paused \
	'PATCH response body shows status=paused'

# No sleep — IMMEDIATE GET. Must see the post-mutation value.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_json_eq '.status' paused \
	'IMMEDIATE GET after PATCH paused shows status=paused (no stale cache)'

# 5b. PATCH status=resumed. Same invariant in the opposite direction.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"status":"resumed"}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "PATCH /downloads/{hash} status=resumed → 200"
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
# Resumed maps back to one of the live statuses (downloading / waiting).
RESUMED=$(printf '%s' "$CURL_BODY" | jq -r '.status')
if [ "$RESUMED" = "paused" ]; then
	_fail "IMMEDIATE GET after PATCH resumed" \
		"still shows status=paused (stale cache)"
else
	_pass "IMMEDIATE GET after PATCH resumed shows non-paused (status=$RESUMED)"
fi

# 5c. PATCH priority=release. Response + immediate GET both show
# release.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"release"}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "PATCH priority=release → 200"
_assert_json_eq '.priority' release 'PATCH response shows priority=release'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_json_eq '.priority' release \
	'IMMEDIATE GET after PATCH priority=release shows priority=release'

# 5d. Combined PATCH — status + priority + category in one body.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"status":"paused","priority":"low","category":0}' \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "PATCH combined (status+priority+category) → 200"
_assert_json_eq '.status'   paused 'combined PATCH response status=paused'
_assert_json_eq '.priority' low    'combined PATCH response priority=low'
_assert_json_eq '.category' 0      'combined PATCH response category=0'
_curl -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_json_eq '.status'   paused 'IMMEDIATE GET after combined PATCH status=paused'
_assert_json_eq '.priority' low    'IMMEDIATE GET after combined PATCH priority=low'

# --- 6. PATCH error paths. -----------------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 400 "PATCH empty body → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"bogus"}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 400 "PATCH unknown priority enum → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"status":"flapped"}' "$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 400 "PATCH unknown status enum → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"status":"paused"}' "$HOST/api/v0/downloads/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "PATCH unknown hash → 404"

# --- 7. Restore the pre-mutation state so the Ubuntu ISO ends up
# in roughly the state it started in. (5b's smoke will clean it up
# via DELETE.)
RESTORE_STATUS=$SAVED_STATUS
case "$RESTORE_STATUS" in
	downloading|waiting|hashing|completing|allocating) RESTORE_STATUS=resumed ;;
	paused|completed|erroneous|insufficient_disk) RESTORE_STATUS=paused ;;
	*) RESTORE_STATUS=resumed ;;
esac
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"status\":\"$RESTORE_STATUS\",\"priority\":\"$SAVED_PRIORITY\",\"category\":$SAVED_CATEGORY}" \
	"$HOST/api/v0/downloads/$TEST_HASH"
_assert_status 200 "PATCH (restore pre-mutation state) → 200"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
