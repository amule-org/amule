#!/usr/bin/env bash
#
# amuleapi 01-version-and-errors — daemon skeleton smoke. Asserts the single
# `/api/v0/version` endpoint and the error-shape envelope.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-test &
#   ./01-version-and-errors.sh
#
# Environment:
#   HOST=localhost:4713   amuleapi endpoint (default port)
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_01_version_and_errors_body.XXXXXX)
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
	resp=$(curl -s --max-time 10 \
		-o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
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
	_die "jq is required for JSON assertions. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 01-version-and-errors smoke @ $HOST"

# 1. GET /api/v0/version → 200 + JSON with name=amuleapi, api_version=v0.
_curl "$HOST/api/v0/version"
_assert_status 200 "GET /api/v0/version returns 200"
_assert_json_eq '.service'     amuleapi '/api/v0/version reports service=amuleapi'
_assert_json_eq '.api_version' v0       '/api/v0/version reports api_version=v0'

# 2. amuleapi_version field — non-empty. On release builds it's
#    e.g. "3.0.1"; on the `master` line it's the literal "GIT"
#    (PACKAGE_VERSION default in the top-level CMakeLists.txt).
#    Pinning a shape would force the smoke to know which kind of
#    build it's poking at, so just assert the field is populated.
_assert_json_eq '.amuleapi_version | length > 0' \
	true '/api/v0/version reports a non-empty amuleapi_version'

# 2b. daemon_version field — the connected amuled's version, taken from
#     the EC_TAG_SERVER_VERSION handshake tag. Distinct from
#     amuleapi_version (which is amuleapi's own build). The regression
#     amuled is a live modern build that always advertises the tag, so
#     assert it's populated. (Against a daemon old enough to omit the
#     tag, or before EC connects, this field is legitimately empty.)
_assert_json_eq '.daemon_version | length > 0' \
	true '/api/v0/version reports a non-empty daemon_version'

# 2c. update object. The identity fields above are unauthenticated because this
#     is the version-negotiation probe (liveness is /health's job), but `update`
#     reports whether THIS daemon is running an outdated build, which an
#     unauthenticated caller on a reachable interface has no business learning.
#     Absent without credentials, present with them.
_assert_json_eq '. | has("update")' false \
	'/api/v0/version omits the update object when unauthenticated'

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login" \
	| jq -r '.token // empty')
if [ -z "$ADMIN_TOKEN" ]; then
	# Cookie-session build: fall back to the jar so the authenticated half
	# still runs rather than silently reporting a pass it never made.
	JAR=$(mktemp)
	curl -s -c "$JAR" -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login" >/dev/null
	AUTH=(-b "$JAR")
else
	AUTH=(-H "Authorization: Bearer $ADMIN_TOKEN")
fi

#     Whether the daemon has actually completed a check depends on its build
#     (ENABLE_VERSION_CHECK) and network access, so assert the shape (keys +
#     types) rather than concrete values.
_curl "${AUTH[@]}" "$HOST/api/v0/version"
_assert_status 200 "GET /api/v0/version (authenticated) returns 200"
_assert_json_eq '.update | type' object '/api/v0/version has an update object when authenticated'
_assert_json_eq '.update.check_enabled | type' boolean 'update.check_enabled is boolean'
_assert_json_eq '.update.checked | type' boolean 'update.checked is boolean'
# latest_version is null until a check completes (R10), so it is string-or-null
# rather than always-string -- the same shape as its two siblings.
_assert_json_eq '(.update.latest_version | type) as $t | $t == "string" or $t == "null"' \
	true 'update.latest_version is string or null'
_assert_json_eq '.update | has("available")' true 'update has available'
_assert_json_eq '.update | has("last_checked_at")' true 'update has last_checked_at'

# 2d. /health — the liveness probe. The surface had none, so /version was being
#     used as one: a document whose body changes over time and whose name says
#     something else. Unauthenticated like /version, no EC roundtrip, and always
#     200 while the server answers, so a probe never restarts a healthy process
#     because amuled went away. Readiness is in the body instead.
_curl "$HOST/api/v0/health"
_assert_status 200 "GET /api/v0/health returns 200 unauthenticated"
_assert_json_eq '.status' ok '/health reports status=ok'
_assert_json_eq '.ec_connected | type' boolean '/health reports ec_connected'
_assert_json_eq '.snapshot | type' boolean '/health reports snapshot'

_curl -I "$HOST/api/v0/health"
_assert_status 200 "HEAD /api/v0/health returns 200"

_curl -X POST "$HOST/api/v0/health"
_assert_status 405 "POST /api/v0/health yields 405"
_assert_json_eq '.error.code' method_not_allowed '/health 405 carries method_not_allowed'
# _curl captures the body and status only, so read the header directly rather
# than asserting against a variable that does not exist.
ALLOW=$(curl -s -o /dev/null -D - --max-time 10 -X POST "$HOST/api/v0/health" \
	| tr -d '\r' | awk -F': ' 'tolower($1)=="allow"{print $2}')
case "$ALLOW" in
*GET*HEAD*) _pass "/health 405 carries Allow: $ALLOW" ;;
*) _fail "/health 405 Allow header" "got [$ALLOW]" ;;
esac

# 3. Method other than GET/HEAD → 405 with the canonical error envelope.
_curl -X DELETE "$HOST/api/v0/version"
_assert_status 405 "DELETE /api/v0/version yields 405"
_assert_json_eq '.error.code' method_not_allowed \
	'/api/v0/version 405 carries error.code=method_not_allowed'

# 4. Unknown route → 404 with the canonical error envelope.
_curl "$HOST/api/v0/does-not-exist"
_assert_status 404 "GET /api/v0/does-not-exist yields 404"
_assert_json_eq '.error.code' not_found \
	'404 carries error.code=not_found'

# 5. HEAD /api/v0/version — same status code as GET, no body required.
_curl -I "$HOST/api/v0/version"
_assert_status 200 "HEAD /api/v0/version returns 200"

# 6. POST /api/v0/version/check is admin-only: unauthenticated → 401. (The
#    admin-authenticated happy path — 202 started / 429 throttled — is
#    exercised where an admin token is available.)
_curl -X POST "$HOST/api/v0/version/check"
_assert_status 401 "POST /api/v0/version/check without auth yields 401"

# 6b. The throttled check answers its own code, not the auth limiter's.
#     Both are 429, and a client that cannot tell them apart has to read a
#     throttled update check as a lost session -- which is exactly what the Web
#     UI did, logging the user out on "Check now".
#
#     Two POSTs back to back: the daemon's cooldown is 60 s and its startup
#     check already consumed one attempt, so whichever way the first lands, the
#     second is inside the window. Deterministic regardless of daemon uptime.
_curl "${AUTH[@]}" -X POST "$HOST/api/v0/version/check"
_curl "${AUTH[@]}" -X POST "$HOST/api/v0/version/check"
_assert_status 429 "second POST /version/check inside the cooldown yields 429"
# Asserted as the exact code rather than "not rate_limited": the negative form
# also passes on an `unauthorized` body, so it would go green on a broken
# request instead of catching it.
_assert_json_eq '.error.code' version_check_throttled \
	'throttled /version/check carries error.code=version_check_throttled'

# 7. GET /api/v0/version/check → 405 (POST only).
_curl "$HOST/api/v0/version/check"
_assert_status 405 "GET /api/v0/version/check yields 405"
_assert_json_eq '.error.code' method_not_allowed \
	'/api/v0/version/check GET 405 carries error.code=method_not_allowed'

# 8. An unauthenticated /version must NOT spend the generic-401 budget.
#    Emitting `update` only to an authenticated caller is the only reason this
#    endpoint authenticates at all, so a request with no credential is its
#    documented unauthenticated use rather than an auth failure. Counted, it
#    would let an anonymous poller -- or, behind a reverse proxy, one poller on
#    the address every client shares -- fill the bucket in 30 requests and lock
#    real sessions out of the entire authenticated surface for five minutes.
#    Defaults: TokenFailureThreshold=30 within TokenFailureWindowSeconds=60.
#
#    Deliberately the last check in this file: if the guard regresses, the
#    lockout it arms would poison every assertion after it.
i=0
while [ "$i" -lt 35 ]; do
	curl -s -o /dev/null --max-time 10 "$HOST/api/v0/version"
	i=$((i + 1))
done
# /auth/session rather than /status: authenticated, but with no EC dependency
# that could answer 503 and mask the 429 this is looking for.
_curl "${AUTH[@]}" "$HOST/api/v0/auth/session"
_assert_status 200 \
	"35 anonymous /version requests do not rate-limit an authenticated caller"

# Summary.
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
