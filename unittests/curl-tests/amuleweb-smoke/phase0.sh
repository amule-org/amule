#!/usr/bin/env bash
#
# Amuleweb smoke test — runs against a stock amuleweb (PHP frontend on
# port 4711, default template) and checks the four pages that exercise
# the legacy path. This branch (the amuleapi work) explicitly does NOT
# touch amuleweb's source; this script's job is to catch any
# unintended cross-cut — e.g. someone misconfigures the CMake graph
# such that the legacy binary changes behaviour, or a libwebcommon
# header bleeds an include into a PHP TU.
#
# Usage:
#   amuleweb --no-php-checks &     # start amuleweb on :4711
#   ./phase0.sh                    # asserts the four landing pages
#
# Environment:
#   HOST=localhost:4711       amuleweb endpoint
#   PASS=x                    amuleweb full-access password (matches
#                             amuleweb's `--admin-pass` / remote.conf)
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4711}
PASS=${PASS:-x}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleweb_smoke_body.XXXXXX)
CURL_HEADERS_FILE=$(mktemp -t amuleweb_smoke_headers.XXXXXX)
COOKIE_JAR=$(mktemp -t amuleweb_smoke_cookies.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEADERS_FILE" "$COOKIE_JAR"' EXIT

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
	resp=$(curl -s --compressed --max-time 10 \
		-b "$COOKIE_JAR" -c "$COOKIE_JAR" \
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

_assert_body_contains() {
	local needle=$1 label=$2
	if printf '%s' "$CURL_BODY" | grep -q -- "$needle"; then
		_pass "$label"
	else
		_fail "$label" "needle '$needle' not found" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

# Preflight: amuleweb up?
if ! curl -s -o /dev/null --max-time 2 "$HOST/" 2>/dev/null; then
	_die "amuleweb at $HOST is not reachable. Start amuled + amuleweb first."
fi

echo "amuleweb smoke @ $HOST"

# 1. /login.php — the unauthenticated landing page. The PHP template
#    renders the login form; we just need a 200 + a hint that the form
#    rendered. amuleweb's default template emits the `<input type="password"
#    name="pass"` element, which is what the user actually types into.
_curl "$HOST/login.php"
_assert_status 200 "GET /login.php returns 200"
_assert_body_contains 'name="pass"' "/login.php renders password input"

# 2. /amuleweb-main-dload.php (downloads list) before login → must
#    redirect / 401 / serve the login page. amuleweb's default template
#    redirects to /login.php on missing session, which curl follows
#    with -L; without -L we get a 302. Either is acceptable for the
#    smoke check; we just want NOT 5xx.
_curl "$HOST/amuleweb-main-dload.php"
case "$CURL_STATUS" in
	200|302|401) _pass "GET /amuleweb-main-dload.php pre-login (HTTP $CURL_STATUS)" ;;
	*)           _fail "GET /amuleweb-main-dload.php pre-login" \
	                 "unexpected status $CURL_STATUS" ;;
esac

# 3. POST /login.php with the configured password → 200 + sets a
#    PHP session cookie. amuleweb's session cookie name is `amuleweb_*`.
_curl -X POST -d "pass=$PASS" "$HOST/login.php"
_assert_status 200 "POST /login.php with admin password"
if grep -q '^amuleweb_' "$COOKIE_JAR" 2>/dev/null \
   || grep -qi 'session' "$COOKIE_JAR" 2>/dev/null; then
	_pass "POST /login.php sets a session cookie"
else
	_fail "POST /login.php sets a session cookie" \
		"no session cookie found in jar" \
		"jar: $(cat "$COOKIE_JAR" 2>/dev/null | head -c 400)"
fi

# 4. /amuleweb-main-dload.php after login → 200 + table markup. The
#    default template emits a `<table` for the download list. If
#    amuleweb's PHP path is broken (the carve-out scenario this script
#    guards against), this is where it shows up.
_curl "$HOST/amuleweb-main-dload.php"
_assert_status 200 "GET /amuleweb-main-dload.php post-login"
_assert_body_contains '<table' "/amuleweb-main-dload.php renders a table"

# Summary.
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
