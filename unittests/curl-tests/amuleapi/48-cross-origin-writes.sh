#!/usr/bin/env bash
#
# amuleapi 48-cross-origin-writes — cookie-authenticated writes a browser marks
# as cross-origin are refused.
#
# SameSite=Strict does not stop a page on another port of the same host, so a
# POST/PUT/PATCH/DELETE without a bearer token gets 403 `forbidden` when
# Sec-Fetch-Site is present and is neither `same-origin` nor `none`, or, without
# it, when Origin names another host. Bearer requests, reads, and requests with
# neither header are not affected. The write used is POST /shared_reload, which
# only schedules a re-walk of the shares.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
API="$HOST/api/v1"
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_48_body.XXXXXX)
COOKIE_JAR=$(mktemp -t amuleapi_48_cookies.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$COOKIE_JAR"' EXIT

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

_assert_error_code() {
	local expected=$1 label=$2 actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r '.error.code' 2>/dev/null)
	if [ "$actual" = "$expected" ]; then
		_pass "$label"
	else
		_fail "$label" "expected error code $expected, got $actual" "body: $CURL_BODY"
	fi
}

# A page on another port of the same host, as the browser describes it.
OTHER_PORT=(-H "Origin: http://127.0.0.1:4711" -H "Sec-Fetch-Site: same-site")

echo "== 48 cross-origin writes"

_curl -c "$COOKIE_JAR" -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$API/auth/login"
[ "$CURL_STATUS" = "200" ] || _die "admin cookie login failed (HTTP $CURL_STATUS)"
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$API/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "admin token login failed"

_curl -b "$COOKIE_JAR" -X POST "${OTHER_PORT[@]}" -H "Content-Type: text/plain" "$API/shared_reload"
_assert_status 403 "cookie POST from another port of the same host is refused"
_assert_error_code forbidden "the refusal is a forbidden error"

_curl -b "$COOKIE_JAR" -X POST -H "Origin: https://evil.example" -H "Sec-Fetch-Site: cross-site" \
	"$API/shared_reload"
_assert_status 403 "cookie POST from another site is refused"

_curl -b "$COOKIE_JAR" -X POST -H "Origin: http://127.0.0.1:4711" "$API/shared_reload"
_assert_status 403 "cookie POST with a foreign Origin and no Sec-Fetch-Site is refused"

_curl -b "$COOKIE_JAR" -X POST -H "Origin: http://$HOST" -H "Sec-Fetch-Site: same-origin" \
	"$API/shared_reload"
_assert_status 202 "cookie POST from the API's own origin is accepted"

_curl -b "$COOKIE_JAR" -X POST "$API/shared_reload"
_assert_status 202 "cookie POST without browser headers is accepted"

_curl -X POST "${OTHER_PORT[@]}" -H "Authorization: Bearer $TOKEN" "$API/shared_reload"
_assert_status 202 "bearer POST is accepted whatever the browser headers say"

_curl -b "$COOKIE_JAR" "${OTHER_PORT[@]}" "$API/status"
_assert_status 200 "cookie GET from another port is not affected"

echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
