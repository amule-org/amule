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

CURL_MAX_TIME=30
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

COOKIE_JAR=$(mktemp -t amuleapi_48_cookies.XXXXXX)
_on_exit 'rm -f "$COOKIE_JAR"'

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
TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] || _die "admin token login failed"

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

_summary
