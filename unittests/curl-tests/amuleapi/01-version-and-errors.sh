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
_assert_json_eq '.name'        amuleapi '/api/v0/version reports name=amuleapi'
_assert_json_eq '.api_version' v0       '/api/v0/version reports api_version=v0'

# 2. amule_version field — non-empty. On release builds it's
#    e.g. "3.0.1"; on the `master` line it's the literal "GIT"
#    (PACKAGE_VERSION default in the top-level CMakeLists.txt).
#    Pinning a shape would force the smoke to know which kind of
#    build it's poking at, so just assert the field is populated.
_assert_json_eq '.amule_version | length > 0' \
	true '/api/v0/version reports a non-empty amule_version'

# 2b. daemon_version field — the connected amuled's version, taken from
#     the EC_TAG_SERVER_VERSION handshake tag. Distinct from
#     amule_version (which is amuleapi's own build). The regression
#     amuled is a live modern build that always advertises the tag, so
#     assert it's populated. (Against a daemon old enough to omit the
#     tag, or before EC connects, this field is legitimately empty.)
_assert_json_eq '.daemon_version | length > 0' \
	true '/api/v0/version reports a non-empty daemon_version'

# 2c. update object — version-check info relayed from the daemon. Whether the
#     daemon has actually completed a check depends on its build
#     (ENABLE_VERSION_CHECK) and network access, so assert the shape (keys +
#     types) rather than concrete values. update_available / last_checked are
#     boolean|null / number|null; only assert their presence.
_assert_json_eq '.update | type' object '/api/v0/version has an update object'
_assert_json_eq '.update.check_enabled | type' boolean 'update.check_enabled is boolean'
_assert_json_eq '.update.checked | type' boolean 'update.checked is boolean'
_assert_json_eq '.update.latest_version | type' string 'update.latest_version is string'
_assert_json_eq '.update | has("update_available")' true 'update has update_available'
_assert_json_eq '.update | has("last_checked")' true 'update has last_checked'

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

# 7. GET /api/v0/version/check → 405 (POST only).
_curl "$HOST/api/v0/version/check"
_assert_status 405 "GET /api/v0/version/check yields 405"
_assert_json_eq '.error.code' method_not_allowed \
	'/api/v0/version/check GET 405 carries error.code=method_not_allowed'

# Summary.
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
