#!/usr/bin/env bash
#
# amuleapi 36-shared-reload — POST /shared_reload.
#
# Endpoint:
#   POST /api/v0/shared_reload   → 202, no `ok` field
#
# amuled schedules a re-walk of every configured share root and answers
# immediately, so the call is accepted (202), never completed (200). The walk
# starts on amuled's next Process() tick and its result is only ever visible
# as amule log lines ("Reloading shared files..." then "Found N known shared
# files"), which a client reads through /logs/amule or the SSE log channel.
# There is therefore nothing to assert about the outcome here; the contract
# under test is the accept path, its guards, and that repeated calls coalesce
# rather than erroring or queueing up.
#
# Why this endpoint has its own smoke: the reload used to run inline in
# amuled's EC handler. Because amuleapi's EC lane is a single serialised
# worker, that blocked roundtrip held the in-flight slot for the whole walk
# and turned unrelated endpoints into 503s. The elapsed-time check below is a
# smoke for that, not a proof — on a small test library even a synchronous
# walk finishes quickly, so it only catches a gross regression.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_36_shared_reload_body.XXXXXX)
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
	resp=$(curl -s --max-time 30 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
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

# A 202 from shared_reload carries a body only when amuled had
# something to say: `{"message": ...}` if it did, no body at all if it did not
# (the empty `{}` was dropped so this matches the URL-fetch triggers). Either is correct; a constant `ok` field is not.
_assert_no_body_or_message() {
	local what=$1
	if [ -z "$CURL_BODY" ]; then
		_pass "$what: 202 with no body (amuled reported nothing)"
	elif echo "$CURL_BODY" | jq -e '(has("ok") | not) and (.message | type == "string")' >/dev/null 2>&1; then
		_pass "$what: 202 body carries amuled's message and no constant ok"
	else
		_fail "$what 202 body" "expected no body or {message}, got: $CURL_BODY"
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

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 36-shared-reload smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth guards. -----------------------------------------------
_curl -X POST "$HOST/api/v0/shared_reload"
_assert_status 401 "POST /shared_reload (no creds) → 401"

if [ "$HAVE_GUEST" -eq 1 ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/shared_reload"
	_assert_status 403 "POST /shared_reload (guest) → 403"
else
	echo "    info: no guest password configured; skipping the 403 guard check"
fi

# --- 2. Accept path. -----------------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared_reload"
_assert_status 202 "POST /shared_reload (admin) → 202"
_assert_no_body_or_message "/shared_reload"

# --- 3. Repeated calls coalesce. -----------------------------------
# A second request while the first is still pending (or its walk running)
# must be accepted the same way, not rejected as a conflict and not queued
# into a second walk. Only the status is observable from here; that the two
# collapse into one walk is amuled-side and shows up in its log.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared_reload"
_assert_status 202 "POST /shared_reload again immediately → 202"
_assert_no_body_or_message "second /shared_reload"

# --- 4. The reply does not wait for the walk. ----------------------
# Generous bound: this is a smoke against a regression to the old inline
# behaviour, not a timing guarantee. SimpleConnControlOp also runs an inline
# RefresherTick, so a handful of EC roundtrips are included in the figure.
START=$(date +%s)
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared_reload"
ELAPSED=$(( $(date +%s) - START ))
_assert_status 202 "POST /shared_reload (timed) → 202"
if [ "$ELAPSED" -le 10 ]; then
	_pass "/shared_reload returned in ${ELAPSED}s (does not block on the walk)"
else
	_fail "/shared_reload returned in ${ELAPSED}s" \
		"expected the reply to be scheduled, not to wait for the directory walk"
fi

# --- 5. The API stays usable while the walk runs. ------------------
# The reload request must not be the thing that occupies the EC service's
# in-flight slot for the walk's duration; if it were, these would 503.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/status"
_assert_status 200 "GET /status right after a reload → 200 (not 503)"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
_assert_status 200 "GET /shared right after a reload → 200"
_assert_json_eq '.shared | type' array "/shared still serves a coherent list"

# --- 6. Method gate. -----------------------------------------------
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared_reload"
_assert_status 405 "GET /shared_reload → 405"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared_reload"
_assert_status 405 "DELETE /shared_reload → 405"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
