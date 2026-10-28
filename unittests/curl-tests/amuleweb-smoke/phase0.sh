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
#   GUEST_PASS=               guest password; set it to run the guest checks
#   GUEST_DENIED=0            1 when amuleweb runs with --deny-guest
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
GUEST_JAR=$(mktemp -t amuleweb_smoke_guest.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEADERS_FILE" "$COOKIE_JAR" "$GUEST_JAR"' EXIT

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

_assert_body_not_contains() {
	local needle=$1 label=$2
	if printf '%s' "$CURL_BODY" | grep -q -- "$needle"; then
		_fail "$label" "needle '$needle' unexpectedly found"
	else
		_pass "$label"
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

# 5. Guest role, only when GUEST_PASS is set. A guest is read-only, so
#    footer.php must not queue its ed2k link; the admin session then
#    queues the same link as a control and cancels it again. With
#    GUEST_DENIED=1 the guest password must not log in at all.
if [ -n "${GUEST_PASS:-}" ]; then
	ADMIN_JAR=$COOKIE_JAR
	COOKIE_JAR=$GUEST_JAR
	_curl -X POST -d "pass=$GUEST_PASS" "$HOST/login.php"
	if [ "${GUEST_DENIED:-0}" = 1 ]; then
		_assert_body_contains 'name="pass"' "guest login refused while guest access is denied"
		COOKIE_JAR=$ADMIN_JAR
	else
		_assert_body_not_contains 'name="pass"' "guest login accepted"
		GUEST_HASH=0123456789ABCDEF0123456789ABCDEF
		GUEST_FILE=amuleweb-guest-smoke.bin
		LINK="ed2k%3A%2F%2F%7Cfile%7C$GUEST_FILE%7C1048576%7C$GUEST_HASH%7C%2F"
		_curl "$HOST/footer.php?Submit=Download+link&selectcat=all&ed2klink=$LINK"
		_assert_status 200 "guest GET /footer.php with an ed2k link"
		COOKIE_JAR=$ADMIN_JAR
		sleep 1
		_curl "$HOST/amuleweb-main-dload.php"
		_assert_body_not_contains "$GUEST_FILE" "guest footer.php did not add a download"
		_curl "$HOST/footer.php?Submit=Download+link&selectcat=all&ed2klink=$LINK"
		sleep 1
		_curl "$HOST/amuleweb-main-dload.php"
		_assert_body_contains "$GUEST_FILE" "admin footer.php adds the same download"
		_curl "$HOST/amuleweb-main-dload.php?command=cancel&$GUEST_HASH=on"
	fi
fi

# Summary.
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
