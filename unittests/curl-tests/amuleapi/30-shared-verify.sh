#!/usr/bin/env bash
#
# amuleapi 30-shared-verify — POST /shared/{hash}/verify.
#
# Endpoint:
#   POST /api/v0/shared/{hash}/verify   → 202 {"ok":true}
#
# Re-hashes a shared file against its on-disk data (EC_OP_VERIFY_LOCAL_DATA).
# amuled queues a CVerifyLocalDataTask and answers immediately, so the call
# is accepted (202), never completed (200) — the verdict is only ever emitted
# as an amule log line ("Verify Local Data (...): Result OK" / "ERRORS
# FOUND!"), which a client reads back through /logs/amule or the SSE log
# channel. There is therefore nothing to assert about the outcome here; the
# contract under test is the accept path and its guards.
#
# Partfiles are rejected with 409 partfile_unsupported: the hashing task
# bails out on IsPartFile(), so accepting one would promise a report that
# never arrives.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}
AMULE_SHARED_DIR=${AMULE_SHARED_DIR:-}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_30_shared_verify_body.XXXXXX)
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

echo "amuleapi 30-shared-verify smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# Verify only applies to completed knownfiles, so the fixture path is the
# same one 17-shared-priority-patch uses: plant a real file in a directory
# amuled shares and let it hash in. Reused across runs once it exists.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')

if [ "$COUNT" = "0" ]; then
	[ -n "$AMULE_SHARED_DIR" ] \
		|| _die "set AMULE_SHARED_DIR to a directory amuled shares so the smoke can plant a fixture"
	FIXTURE="$AMULE_SHARED_DIR/amuleapi-regtest-shared.dat"
	if [ ! -f "$FIXTURE" ]; then
		head -c 1048576 /dev/urandom > "$FIXTURE" \
			|| _die "cannot write fixture to AMULE_SHARED_DIR=$AMULE_SHARED_DIR"
	fi
	echo "    info: planted fixture $FIXTURE; reloading shares"
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/shared/reload"
	for _ in $(seq 1 30); do
		sleep 1
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
		COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
		[ "$COUNT" != "0" ] && break
	done
	[ "$COUNT" != "0" ] \
		|| _die "fixture planted in $AMULE_SHARED_DIR but never appeared in /shared after reload"
fi

# Pick a completed entry — a shared partfile is rejected by design, so it
# can't stand in for the happy path. The list object carries no partfile
# flag; the detail object's `path` reads "[PartFile]" exactly while a file
# is genuinely incomplete (#417), so classify through that. The same sweep
# picks up a partfile for the 409 guard below, when the library has one.
TEST_HASH=""
PART_HASH=""
for h in $(printf '%s' "$CURL_BODY" | jq -r '.shared[].hash' | head -20); do
	P=$(curl -s --max-time 10 -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/shared/$h" | jq -r '.path')
	if [ "$P" = "[PartFile]" ]; then
		[ -z "$PART_HASH" ] && PART_HASH=$h
	else
		[ -z "$TEST_HASH" ] && TEST_HASH=$h
	fi
	[ -n "$TEST_HASH" ] && [ -n "$PART_HASH" ] && break
done
[ -n "$TEST_HASH" ] || _die "no completed (non-partfile) shared file available to verify"
echo "    info: verifying hash=$TEST_HASH"

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X POST "$HOST/api/v0/shared/$TEST_HASH/verify"
_assert_status 401 "POST /shared/{hash}/verify (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/shared/$TEST_HASH/verify"
	_assert_status 403 "POST /shared/{hash}/verify (guest) → 403"
fi

# --- 2. Happy path: accepted, not completed. -----------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/$TEST_HASH/verify"
_assert_status 202 "POST /shared/{hash}/verify → 202"
_assert_json_eq '.ok' true "verify response .ok == true"

# Uppercase hash resolves the same file (lookup lowercases the capture).
UPPER_HASH=$(printf '%s' "$TEST_HASH" | tr 'a-f' 'A-F')
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/$UPPER_HASH/verify"
_assert_status 202 "POST /shared/{HASH}/verify (uppercase) → 202"

# --- 3. Unknown hash → 404. ----------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/00000000000000000000000000000000/verify"
_assert_status 404 "POST /shared/{unknown}/verify → 404"
_assert_json_eq '.error.code' not_found "unknown hash → error.code=not_found"

# --- 4. Method gate. -----------------------------------------------
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/$TEST_HASH/verify"
_assert_status 405 "GET /shared/{hash}/verify → 405"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{}' \
	"$HOST/api/v0/shared/$TEST_HASH/verify"
_assert_status 405 "PATCH /shared/{hash}/verify → 405"

# --- 5. Partfile guard (only when the library has one). ------------
# A partfile shows up in /shared once ≥1 part has completed. Classified in
# the sweep above. Skipped rather than forced, since planting a
# half-finished download isn't reproducible in a smoke run.
if [ -n "$PART_HASH" ]; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/shared/$PART_HASH/verify"
	_assert_status 409 "POST /shared/{partfile}/verify → 409"
	_assert_json_eq '.error.code' partfile_unsupported \
		"partfile → error.code=partfile_unsupported"
else
	echo "    info: no shared partfile present; skipping the 409 guard check"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
