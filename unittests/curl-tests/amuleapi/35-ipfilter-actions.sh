#!/usr/bin/env bash
#
# amuleapi 35-ipfilter-actions — POST /ipfilter/reload, POST /ipfilter/update.
#
# Endpoints:
#   POST /api/v0/ipfilter/reload   → 202, no `ok` field
#   POST /api/v0/ipfilter/update   → 202, no body at all
#
# The two actions the desktop Security page's "Reload List" and "Update now"
# buttons drive, as EC opcodes that have existed for years. Both are accepted,
# never completed: amuled queues the work and reports only through its log
# ("IP filter is ready", "Failed to download ipfilter.dat from <url>"), so
# there is nothing to assert about the outcome — the contract under test is
# the accept path, the URL resolution and the guards.
#
# Neither reports anything the caller did not already have. update answers
# with no body at all: the URL it used to echo came straight back out of the
# request, and where it came from preferences instead the caller reads it from
# GET /preferences, which section 5 already does. reload shares the
# connection-control shape, so its body is an object carrying whatever status
# string amuled returned - for this opcode, none - and never a constant `ok`.
#
# The update happy path deliberately uses an unroutable URL: the download
# fails asynchronously, amuled logs it and keeps the current filter live, so
# the run never fetches a real multi-megabyte list and never swaps the
# operator's ipfilter.dat. Reload is side-effect-free by construction (it
# re-reads the files already on disk).
#
# security.ipfilter_update_url is written by an explicit-URL update (that is
# the behaviour under test), so the original value is captured up front and
# restored at the end.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

# Unroutable by design (discard port on loopback): amuled's downloader fails
# fast and nothing is fetched or swapped.
TEST_URL="http://127.0.0.1:9/ipfilter.dat"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_35_ipfilter_body.XXXXXX)
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

_assert_body_empty() {
	local label=$1
	if [ -z "$CURL_BODY" ]; then
		_pass "$label"
	else
		_fail "$label" "expected an empty body, got: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

# Read one preference out of GET /preferences.
_pref() {
	curl -s --max-time 10 -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/preferences" | jq -r "$1"
}

# Write security.ipfilter_update_url and wait for the snapshot to catch up —
# the resolution path under test reads amuleapi's snapshot, not amuled.
_set_url() {
	curl -s -o /dev/null -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"security\":{\"ipfilter_update_url\":\"$1\"}}" "$HOST/api/v0/preferences"
	for _ in $(seq 1 15); do
		[ "$(_pref '.security.ipfilter_update_url')" = "$1" ] && return 0
		sleep 1
	done
	return 1
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 35-ipfilter-actions smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

ORIGINAL_URL=$(_pref '.security.ipfilter_update_url')
[ "$ORIGINAL_URL" = "null" ] && _die "GET /preferences has no security.ipfilter_update_url"
echo "    info: saved security.ipfilter_update_url=\"$ORIGINAL_URL\" for restore"
restore_url() { _set_url "$ORIGINAL_URL" >/dev/null 2>&1 || true; }
trap 'restore_url; rm -f "$CURL_BODY_FILE"' EXIT

# --- 1. /ipfilter/reload: auth, admin gate, happy path, verbs. -----
_curl -X POST "$HOST/api/v0/ipfilter/reload"
_assert_status 401 "POST /ipfilter/reload (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/ipfilter/reload"
	_assert_status 403 "POST /ipfilter/reload (guest) → 403"
fi

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/ipfilter/reload"
_assert_status 202 "POST /ipfilter/reload → 202"
# amuled answers this one with no status string, so there is nothing to report
# and the reply carries no body -- the same shape /ipfilter/update beside it
# returns, rather than an empty object. When amuled does say something, the
# body is {"message": "..."} (see the connect routes).
if [ -z "$CURL_BODY" ]; then
	_pass "reload with nothing to report returns no body (not an empty object)"
else
	_assert_json_eq '. | has("ok")' false "reload response has no constant ok field"
	_assert_json_eq '. | has("message")' true \
		"a reload body exists only to carry the daemon's message"
fi

_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/ipfilter/reload"
_assert_status 405 "GET /ipfilter/reload → 405"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/ipfilter/reload"
_assert_status 405 "DELETE /ipfilter/reload → 405"

# --- 2. /ipfilter/update: auth + admin gate. -----------------------
_curl -X POST -H "Content-Type: application/json" \
	-d "{\"url\":\"$TEST_URL\"}" "$HOST/api/v0/ipfilter/update"
_assert_status 401 "POST /ipfilter/update (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"url\":\"$TEST_URL\"}" "$HOST/api/v0/ipfilter/update"
	_assert_status 403 "POST /ipfilter/update (guest) → 403"
fi

# --- 3. Body validation. -------------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{"url":""}' \
	"$HOST/api/v0/ipfilter/update"
_assert_status 400 "POST /ipfilter/update empty url → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{"url":123}' \
	"$HOST/api/v0/ipfilter/update"
_assert_status 400 "POST /ipfilter/update non-string url → 400"

# Scheme gate: amuled hands the string to the HTTP downloader, so a
# non-http(s) scheme is rejected here or it fails asynchronously with no
# way to report back.
for BAD in "ftp://example.com/ipfilter.dat" "file:///etc/passwd" "example.com/ipfilter.dat"; do
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"url\":\"$BAD\"}" "$HOST/api/v0/ipfilter/update"
	_assert_status 400 "POST /ipfilter/update rejects scheme: $BAD → 400"
done

# --- 4. No body, no configured URL → 400 (not a silent no-op). -----
# CIPFilter::Update() returns immediately on an empty URL, so accepting
# this would promise work that never happens.
if _set_url ""; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/ipfilter/update"
	_assert_status 400 "POST /ipfilter/update no body, no configured URL → 400"
	_assert_json_eq '.error.code' bad_request "no URL available → error.code=bad_request"
else
	_fail "clear security.ipfilter_update_url" "PATCH did not take effect within 15 s"
fi

# --- 5. Explicit URL: accepted, echoed, and persisted. -------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"url\":\"$TEST_URL\"}" "$HOST/api/v0/ipfilter/update"
_assert_status 202 "POST /ipfilter/update explicit URL → 202"
_assert_body_empty "update sends no body"

# CIPFilter::Update() stores the URL it was handed, the same way the ed2k
# and Kad list downloads do, so the next auto-update at startup uses it.
PERSISTED=""
for _ in $(seq 1 15); do
	PERSISTED=$(_pref '.security.ipfilter_update_url')
	[ "$PERSISTED" = "$TEST_URL" ] && break
	sleep 1
done
if [ "$PERSISTED" = "$TEST_URL" ]; then
	_pass "update persisted the URL into security.ipfilter_update_url"
else
	_fail "update persisted the URL into security.ipfilter_update_url" \
		"expected $TEST_URL, got $PERSISTED" \
		"(needs the core-side CIPFilter::Update() change; an older amuled will not persist)"
fi

# --- 6. No body: the configured URL is what runs. ------------------
#
# Section 4 already pinned the other half of this: with no configured URL and
# no body, the same request is a 400. Reaching 202 here is what says the
# handler fell back to security.ipfilter_update_url, which section 5 just
# proved holds $TEST_URL.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/ipfilter/update"
_assert_status 202 "POST /ipfilter/update no body, configured URL → 202"
_assert_body_empty "bodyless update sends no body"

# An empty JSON object is the same case as no body at all.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{}' "$HOST/api/v0/ipfilter/update"
_assert_status 202 "POST /ipfilter/update {} with configured URL → 202"
_assert_body_empty "{} update sends no body"

# --- 7. Method gate. -----------------------------------------------
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/ipfilter/update"
_assert_status 405 "GET /ipfilter/update → 405"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{}' "$HOST/api/v0/ipfilter/update"
_assert_status 405 "PATCH /ipfilter/update → 405"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
