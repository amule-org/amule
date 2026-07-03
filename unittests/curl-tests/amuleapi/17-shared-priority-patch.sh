#!/usr/bin/env bash
#
# amuleapi 17-shared-priority-patch — PATCH /shared/{hash} priority.
#
# Endpoint:
#   PATCH /api/v0/shared/{hash}
#       body: {priority: <enum>}
#
# Priority input enum (base levels + "auto"):
#   very_low | low | normal | high | release | auto
#
# Output mirrors /downloads: a base `priority` string plus a
# `priority_auto` boolean. Sending "auto" hands level selection to
# amuled (it derives the level from the upload queue, priority_auto=true);
# the combined "*_auto" strings are NOT accepted as input (400).

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_17_shared_priority_patch_body.XXXXXX)
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

echo "amuleapi 17-shared-priority-patch smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# Pick the first shared file for testing — order-independent across
# operator's libraries. /shared is the broader surface (completed
# knownfiles + downloading-and-shared partfiles per Phase 4f).
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')

# Self-provision a fixture when the daemon shares nothing. /shared only
# lists completed knownfiles (a partfile shows up only once a part has
# finished), so a synthetic ed2k link won't do — we must plant a real
# file in a directory amuled shares and let it hash in. AMULE_SHARED_DIR
# (set by run-all.sh) points at that directory, typically amuled's
# Incoming. The fixture is reused across runs once it exists.
if [ "$COUNT" = "0" ]; then
	if [ -z "$AMULE_SHARED_DIR" ]; then
		echo "    info: no shared files and AMULE_SHARED_DIR unset"
		_die "set AMULE_SHARED_DIR to a directory amuled shares so the smoke can plant a fixture"
	fi
	FIXTURE="$AMULE_SHARED_DIR/amuleapi-regtest-shared.dat"
	if [ ! -f "$FIXTURE" ]; then
		head -c 1048576 /dev/urandom > "$FIXTURE" \
			|| _die "cannot write fixture to AMULE_SHARED_DIR=$AMULE_SHARED_DIR"
	fi
	echo "    info: planted fixture $FIXTURE; reloading shares"
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$HOST/api/v0/shared/reload"
	# Wait for amuled to hash the file and surface it in /shared.
	for _ in $(seq 1 30); do
		sleep 1
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
		COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
		[ "$COUNT" != "0" ] && break
	done
	[ "$COUNT" != "0" ] \
		|| _die "fixture planted in $AMULE_SHARED_DIR but never appeared in /shared after reload"
fi
TEST_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.shared[0].hash')
SAVED_PRIORITY=$(printf '%s' "$CURL_BODY" | jq -r '.shared[0].priority')
SAVED_AUTO=$(printf '%s' "$CURL_BODY" | jq -r '.shared[0].priority_auto')
echo "    info: saved hash=$TEST_HASH priority=$SAVED_PRIORITY priority_auto=$SAVED_AUTO"

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 401 "PATCH /shared/{hash} (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X PATCH -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"priority":"high"}' "$HOST/api/v0/shared/$TEST_HASH"
	_assert_status 403 "PATCH /shared/{hash} (guest) → 403"
fi

# --- 2. PATCH priority each bare value + no-stale GET. ------------
for p in low normal high release very_low; do
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"priority\":\"$p\"}" "$HOST/api/v0/shared/$TEST_HASH"
	_assert_status 200 "PATCH priority=$p → 200"
	_assert_json_eq '.priority' "$p" "PATCH response shows priority=$p"
	_assert_json_eq '.priority_auto' false "PATCH priority=$p → priority_auto=false"

	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
	OBS=$(printf '%s' "$CURL_BODY" \
		| jq -r --arg h "$TEST_HASH" \
		  '.shared[] | select(.hash == $h) | .priority')
	if [ "$OBS" = "$p" ]; then
		_pass "IMMEDIATE GET /shared shows priority=$p (no stale)"
	else
		_fail "IMMEDIATE GET /shared priority" \
			"expected $p, got $OBS (stale cache)"
	fi
done

# --- 3. PATCH "auto" → priority_auto=true + a derived base level. --
# amuled derives the concrete level from the upload queue, so we don't
# pin the base string — just assert the flag is set and the reported
# level is a known base enum (never a combined "*_auto" string).
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"auto"}' "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 200 "PATCH priority=auto → 200"
_assert_json_eq '.priority_auto' true "PATCH priority=auto → priority_auto=true"
BASE_PRIO=$(printf '%s' "$CURL_BODY" | jq -r '.priority')
case "$BASE_PRIO" in
	very_low|low|normal|high|release) _pass "PATCH auto → derived base level=$BASE_PRIO" ;;
	*) _fail "PATCH auto base level" "expected a base enum, got '$BASE_PRIO'" ;;
esac

# Immediate GET reflects the auto flag (no stale cache).
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared"
GAUTO=$(printf '%s' "$CURL_BODY" \
	| jq -r --arg h "$TEST_HASH" '.shared[] | select(.hash == $h) | .priority_auto')
if [ "$GAUTO" = "true" ]; then
	_pass "IMMEDIATE GET /shared shows priority_auto=true (no stale)"
else
	_fail "IMMEDIATE GET /shared priority_auto" "expected true, got $GAUTO (stale cache)"
fi

# The combined "*_auto" strings are no longer accepted as input.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"high_auto"}' "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 400 "PATCH removed variant high_auto → 400"

# --- 4. Error paths. ----------------------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"bogus"}' "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 400 "PATCH unknown priority enum → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 400 "PATCH empty body (no priority) → 400"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"low"}' \
	"$HOST/api/v0/shared/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "PATCH unknown hash → 404"

# --- 5. Method gate (POST/DELETE not allowed). --------------------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/$TEST_HASH"
_assert_status 405 "DELETE /shared/{hash} → 405"

# --- 6. Restore saved priority. -----------------------------------
# If the file was on auto, restore "auto" (SAVED_PRIORITY holds the
# derived base level, not the literal "auto"); otherwise restore the
# saved bare level.
if [ "$SAVED_AUTO" = "true" ]; then
	RESTORE=auto
else
	RESTORE=$SAVED_PRIORITY
fi
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"priority\":\"$RESTORE\"}" "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 200 "PATCH (restore saved priority) → 200"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
