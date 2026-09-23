#!/usr/bin/env bash
#
# amuleapi 44-base-path -- serving under a reverse-proxy sub-path.
#
# run-all.sh starts this phase with `[Server]/BasePath=/amule` and StaticRoot
# wired. Wire contract:
#   * `/amule/...` is served as `/...`, and so is the unprefixed form, which is
#     what a proxy that strips the prefix itself sends.
#   * `/amule` without the slash is a 308 to `/amule/`, query kept.
#   * The session cookie is scoped to `/amule/api/v1`, on login and on logout.
#   * Location headers carry the prefix.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
BASE=${BASE_PATH:-/amule}
API="$HOST$BASE/api/v1"
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0
SKIP_COUNT=0

HDR=$(mktemp -t amuleapi_44_hdr.XXXXXX)
BODY=$(mktemp -t amuleapi_44_body.XXXXXX)
JAR=$(mktemp -t amuleapi_44_jar.XXXXXX)
trap 'rm -f "$HDR" "$BODY" "$JAR"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	: > "$HDR"; : > "$BODY"
	curl -sS -o "$BODY" -D "$HDR" "$@" || true
}
_status() { head -1 "$HDR" | awk '{print $2}'; }
_hdr() {
	awk -v n="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" '
		{ l = tolower($0) }
		index(l, n ":") == 1 {
			v = substr($0, length(n) + 2)
			sub(/\r$/, "", v); sub(/^[[:space:]]+/, "", v)
			print v; exit
		}' "$HDR"
}
_expect() {
	local got=$1 want=$2 label=$3
	if [ "$got" = "$want" ]; then
		_pass "$label"
	else
		_fail "$label" "expected '$want', got '$got'"
	fi
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$API/health" 2>/dev/null; then
	_die "amuleapi at $HOST$BASE is not reachable. Is BasePath=$BASE set?"
fi

echo "amuleapi 44-base-path smoke @ $HOST$BASE"

# --- Both spellings reach the API. ---------------------------------
_curl "$API/health"
_expect "$(_status)" 200 "GET $BASE/api/v1/health → 200"
_curl "$HOST/api/v1/health"
_expect "$(_status)" 200 "GET /api/v1/health (proxy stripped the prefix) → 200"

# --- The bare prefix redirects to its directory form. ---------------
_curl "$HOST$BASE"
_expect "$(_status)" 308 "GET $BASE → 308"
_expect "$(_hdr Location)" "$BASE/" "GET $BASE redirects to $BASE/"
_curl "$HOST$BASE?lang=it"
_expect "$(_hdr Location)" "$BASE/?lang=it" "GET $BASE?lang=it keeps the query"
_curl -I "$HOST$BASE"
_expect "$(_status)" 308 "HEAD $BASE → 308"

# --- The WebUI loads under the prefix. ------------------------------
_curl "$HOST$BASE/"
_expect "$(_status)" 200 "GET $BASE/ → 200"
if grep -qi '^content-type: text/html' "$HDR"; then
	_pass "GET $BASE/ serves index.html"
else
	_fail "GET $BASE/ serves index.html" "content-type: $(_hdr Content-Type)"
fi
_curl "$HOST$BASE/js/api.js"
_expect "$(_status)" 200 "GET $BASE/js/api.js → 200"

# --- A segment that only starts with the prefix is not stripped. ----
_curl "$HOST${BASE}x/api/v1/health"
if [ "$(_status)" != "200" ] || ! jq -e . "$BODY" >/dev/null 2>&1; then
	_pass "GET ${BASE}x/api/v1/health does not reach the API"
else
	_fail "GET ${BASE}x/api/v1/health does not reach the API" "got a JSON 200"
fi

# --- The session cookie is scoped to the prefixed API. --------------
_curl -c "$JAR" -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$API/auth/login"
_expect "$(_status)" 200 "POST $BASE/api/v1/auth/login → 200"
COOKIE=$(_hdr Set-Cookie)
case "$COOKIE" in
*"; Path=$BASE/api/v1;"*) _pass "login cookie has Path=$BASE/api/v1" ;;
*) _fail "login cookie has Path=$BASE/api/v1" "got: $COOKIE" ;;
esac

# curl applies the cookie's Path the way a browser does.
_curl -b "$JAR" "$API/status"
_expect "$(_status)" 200 "cookie is sent back on $BASE/api/v1/status"
_curl -b "$JAR" "$HOST/api/v1/status"
_expect "$(_status)" 401 "cookie is not sent outside $BASE/api/v1"

# --- The event stream works under the prefix. -----------------------
: > "$HDR"
curl -sS -m 2 -D "$HDR" -o /dev/null -b "$JAR" "$API/events" 2>/dev/null || true
_expect "$(_status)" 200 "GET $BASE/api/v1/events → 200"
case "$(_hdr Content-Type)" in
text/event-stream*) _pass "GET $BASE/api/v1/events streams" ;;
*) _fail "GET $BASE/api/v1/events streams" "content-type: $(_hdr Content-Type)" ;;
esac

# --- Location carries the prefix. -----------------------------------
_curl -b "$JAR" -X POST -H "Content-Type: application/json" \
	-d '{"query":"amuleapi base path","type":"kad"}' "$API/search"
if [ "$(_status)" = "202" ]; then
	SID=$(jq -r .search_id "$BODY")
	_expect "$(_hdr Location)" "$BASE/api/v1/search/$SID" "POST /search Location has the prefix"
	_curl -b "$JAR" -X DELETE "$API/search/$SID"
else
	_skip "POST /search Location (search not accepted: $(_status))"
fi

# --- Logout clears the cookie on the same path. ---------------------
_curl -b "$JAR" -X POST "$API/auth/logout"
_expect "$(_status)" 204 "POST $BASE/api/v1/auth/logout → 204"
COOKIE=$(_hdr Set-Cookie)
case "$COOKIE" in
*"; Path=$BASE/api/v1;"*"Max-Age=0"*) _pass "logout clears the cookie on Path=$BASE/api/v1" ;;
*) _fail "logout clears the cookie on Path=$BASE/api/v1" "got: $COOKIE" ;;
esac

echo
[ "$SKIP_COUNT" -gt 0 ] && echo "($SKIP_COUNT skipped)"
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
