#!/usr/bin/env bash
#
# amuleapi 02-auth — auth surface. Exercises /api/v0/auth/login,
# /auth/session, /auth/logout against a freshly-started amuleapi
# whose admin password is `adminpass` and whose guest password is
# unset.
#
# Bring-up convention (matches the README in the parent dir):
#   rm -rf /tmp/amuleapi-02-auth && mkdir -p /tmp/amuleapi-02-auth
#   amuleapi --config-dir=/tmp/amuleapi-02-auth --host=127.0.0.1 \
#            --port=4712 --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-02-auth --host=127.0.0.1 \
#            --port=4712 --password=amule &
#   ./02-auth.sh
#
# Environment:
#   HOST=localhost:4713         amuleapi endpoint
#   ADMIN_PASS=adminpass        plaintext admin password
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_02_auth_body.XXXXXX)
CURL_HDR_FILE=$(mktemp -t amuleapi_02_auth_hdr.XXXXXX)
COOKIE_JAR=$(mktemp -t amuleapi_02_auth_cookies.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HDR_FILE" "$COOKIE_JAR"' EXIT

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
		-D "$CURL_HDR_FILE" \
		-o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
	CURL_HDR=$(cat "$CURL_HDR_FILE")
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

_assert_header_contains() {
	local needle=$1 label=$2
	if printf '%s' "$CURL_HDR" | grep -qi -- "$needle"; then
		_pass "$label"
	else
		_fail "$label" "needle '$needle' not in response headers" \
			"headers: $(printf '%s' "$CURL_HDR" | head -c 400)"
	fi
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 02-auth smoke @ $HOST"

# --- 1. Login with wrong password → 401 invalid_credentials. -------
_curl -X POST -H "Content-Type: application/json" \
	-d '{"password":"wrong-password"}' \
	"$HOST/api/v0/auth/login?type=bearer"
_assert_status 401 "POST /auth/login with wrong password → 401"
_assert_json_eq '.error.code' invalid_credentials \
	'401 carries error.code=invalid_credentials'

# --- 2. Login with right password → 200 + JWT + Set-Cookie. --------
_curl -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer"
_assert_status 200 "POST /auth/login with admin password → 200"
_assert_json_eq '.role'                    admin    'login response role=admin'
_assert_json_eq '.token | length > 100'    true     'login response carries a real JWT (>100 chars)'
_assert_json_eq '.expires_at | length'     20       'expires_at is 20-char ISO-8601'
_assert_json_eq '.expires_at_unix | type'  number   'expires_at_unix is numeric'
_assert_json_eq '.jti | length'            22       'jti is 22-char base64url'
_assert_header_contains 'set-cookie: amuleapi_token=' \
	'login sets the amuleapi_token cookie'
_assert_header_contains 'HttpOnly' \
	'cookie carries HttpOnly attribute'
_assert_header_contains 'SameSite=Strict' \
	'cookie carries SameSite=Strict attribute'

# Stash the token for the next blocks.
TOKEN=$(printf '%s' "$CURL_BODY" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "couldn't extract token from login response: $CURL_BODY"

# --- 3. /auth/session with bearer → 200 + role/jti/exp. ------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/auth/session"
_assert_status 200 "GET /auth/session (bearer) → 200"
_assert_json_eq '.role' admin 'session.role=admin (bearer)'
_assert_json_eq '.jti  | length' 22 'session.jti is 22-char (bearer)'
_assert_json_eq '.exp  | length' 20 'session.exp is 20-char ISO-8601 (bearer)'

# --- 4. /auth/session with cookie → same. --------------------------
# Use the cookie jar populated by curl above by re-sending the same
# header verbatim — simulates what a browser would do automatically.
_curl -b "amuleapi_token=$TOKEN" "$HOST/api/v0/auth/session"
_assert_status 200 "GET /auth/session (cookie) → 200"
_assert_json_eq '.role' admin 'session.role=admin (cookie)'

# --- 5. /auth/session with no creds → 401 unauthorized. ------------
_curl "$HOST/api/v0/auth/session"
_assert_status 401 "GET /auth/session (no creds) → 401"
_assert_json_eq '.error.code' unauthorized \
	'no-creds 401 carries error.code=unauthorized'

# --- 6. /auth/session with bogus bearer → 401. ---------------------
_curl -H "Authorization: Bearer not.a.real.jwt" "$HOST/api/v0/auth/session"
_assert_status 401 "GET /auth/session (bogus bearer) → 401"

# --- 7. /auth/logout (bearer) → 200 + clearing Set-Cookie. ---------
_curl -X POST -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/auth/logout"
_assert_status 200 "POST /auth/logout (bearer) → 200"
_assert_json_eq '.ok' true 'logout body acks {ok:true}'
_assert_header_contains 'set-cookie: amuleapi_token=;' \
	'logout sends a clearing Set-Cookie'

# --- 8. /auth/session with same (now revoked) bearer → 401. --------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/auth/session"
_assert_status 401 "GET /auth/session (revoked bearer) → 401"
_assert_json_eq '.error.code' unauthorized \
	'revoked bearer 401 carries error.code=unauthorized'

# --- 9. Method gate. ----------------------------------------------
_curl -X GET "$HOST/api/v0/auth/login?type=bearer"
_assert_status 405 "GET /auth/login → 405 method_not_allowed"
_curl -X GET "$HOST/api/v0/auth/logout"
_assert_status 405 "GET /auth/logout → 405 method_not_allowed"

# --- 10. Bad JSON body on login → 400 bad_request. -----------------
_curl -X POST -H "Content-Type: application/json" \
	-d 'not-even-json' \
	"$HOST/api/v0/auth/login?type=bearer"
_assert_status 400 "POST /auth/login (bad JSON) → 400"
_assert_json_eq '.error.code' bad_request \
	'bad-JSON 400 carries error.code=bad_request'

# --- 11. Rate-limit: 5 wrong passwords (default threshold) → 429 ---
# Defaults from amuleapi.conf: LoginFailureWindowSeconds=60,
# LoginFailureThreshold=5, LoginLockoutSeconds=300.
#
# Boundary check: Check() runs BEFORE the password compare, then
# NoteFailure() runs AFTER a wrong-password reject. So with
# threshold=5:
#   attempts 1-5  → status 401 (bucket grows 1..5; NoteFailure on
#                   the 5th sets lockout_until)
#   attempts 6+   → status 429 (the Check at the top of attempt 6
#                   is the first one that observes lockout_until > now)
# This pins the off-by-one boundary so a regression that arms the
# lockout one attempt early (or late) trips the assertion.
for i in 1 2 3 4; do
	_curl -X POST -H "Content-Type: application/json" \
		-d '{"password":"wrong"}' \
		"$HOST/api/v0/auth/login?type=bearer" > /dev/null
done
# Attempt 5: NoteFailure() arms the lockout AFTER returning 401.
_curl -X POST -H "Content-Type: application/json" \
	-d '{"password":"wrong"}' \
	"$HOST/api/v0/auth/login?type=bearer"
_assert_status 401 "POST /auth/login: 5th failure arms but still returns 401"
# Attempt 6: Check() sees the armed lockout → 429.
_curl -X POST -H "Content-Type: application/json" \
	-d '{"password":"wrong"}' \
	"$HOST/api/v0/auth/login?type=bearer"
_assert_status 429 "POST /auth/login: 6th attempt is the first 429"
_assert_json_eq '.error.code' rate_limited \
	'6th-attempt lockout carries error.code=rate_limited'

# Attempt 7 stays locked.
_curl -X POST -H "Content-Type: application/json" \
	-d '{"password":"wrong"}' \
	"$HOST/api/v0/auth/login?type=bearer" > /dev/null
# The 8th attempt also remains locked.
_curl -X POST -H "Content-Type: application/json" \
	-d '{"password":"wrong"}' \
	"$HOST/api/v0/auth/login?type=bearer"
_assert_status 429 "POST /auth/login after many failures → 429"
_assert_json_eq '.error.code' rate_limited \
	'lockout carries error.code=rate_limited'
_assert_header_contains 'retry-after:' \
	'lockout carries Retry-After header'

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
