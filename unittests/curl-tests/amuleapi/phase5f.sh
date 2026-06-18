#!/usr/bin/env bash
#
# amuleapi Phase 5f — PATCH /shared/{hash} priority.
#
# Endpoint:
#   PATCH /api/v0/shared/{hash}
#       body: {priority: <enum>}
#
# Priority enum mirrors the /shared GET shape (combined wire strings
# including the *_auto variants):
#   very_low | low | normal | high | release | auto
#   very_low_auto | low_auto | normal_auto | high_auto | release_auto
#
# auto-priority is encoded amule-side as `prio + 10`; the handler
# splits the wire string into the raw PR_* code + auto offset.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_phase5f_body.XXXXXX)
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

echo "amuleapi phase 5f smoke @ $HOST"

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
if [ "$COUNT" = "0" ]; then
	echo "    info: no shared files; cannot exercise PATCH path"
	_die "smoke needs at least one shared file"
fi
TEST_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.shared[0].hash')
SAVED_PRIORITY=$(printf '%s' "$CURL_BODY" | jq -r '.shared[0].priority')
echo "    info: saved hash=$TEST_HASH priority=$SAVED_PRIORITY"

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

# --- 3. PATCH auto variants. --------------------------------------
# Pure "auto" amule resolves to a concrete derived enum (e.g.
# "normal_auto" depending on the file's stats), so we don't pin it
# to "auto" verbatim — just assert the response carries SOME *_auto
# variant.
for p in low_auto high_auto; do
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"priority\":\"$p\"}" "$HOST/api/v0/shared/$TEST_HASH"
	_assert_status 200 "PATCH priority=$p → 200"
	_assert_json_eq '.priority' "$p" "PATCH response shows priority=$p"
done
# Bare "auto" → response should be an *_auto variant.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"auto"}' "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 200 "PATCH priority=auto → 200"
RESOLVED=$(printf '%s' "$CURL_BODY" | jq -r '.priority')
case "$RESOLVED" in
	auto|*_auto) _pass "PATCH priority=auto resolved to $RESOLVED (an auto variant)" ;;
	*)           _fail "PATCH priority=auto" \
	                   "expected an *_auto variant, got '$RESOLVED'" ;;
esac

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
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"priority\":\"$SAVED_PRIORITY\"}" "$HOST/api/v0/shared/$TEST_HASH"
_assert_status 200 "PATCH (restore saved priority) → 200"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
