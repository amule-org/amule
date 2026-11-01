# Harness shared by the amuleapi curl phases. Sourced, never run: it lives
# outside the [0-9]*.sh glob CI uses to pick phases.
#
# A phase may set these before sourcing:
#   CURL_MAX_TIME=N   per-request timeout in seconds (default 10)
#   CURL_NONFATAL=1   a transport error sets CURL_STATUS=000 instead of exit 2
#   CURL_AUTO_AUTH=1  _curl sends "${AUTH[@]}" with every request
#
# Environment:
#   HOST=localhost:4713          amuleapi endpoint
#   API=$HOST/api/v1             API root (the offline fixtures pass it)
#   ADMIN_PASS=adminpass         plaintext admin password
#   GUEST_PASS=guestpass         plaintext guest password
#
# Exit codes: 0 all passed, 1 an assertion failed, 2 bring-up error.

. "$(dirname "${BASH_SOURCE[0]}")/amuleapi-bin.sh"

HOST=${HOST:-localhost:4713}
API=${API:-"$HOST/api/v1"}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

# The suite's disposable download. 12, 13, 22, 23, 24 and 26 add and delete
# it; 00 keeps it out of the peer fixture.
TEST_LINK="ed2k://|file|ubuntu-24.04.4-desktop-amd64.iso|6655619072|0031C9CBA65C50DD2015C184B2CA2C88|/"
TEST_HASH="0031c9cba65c50dd2015c184b2ca2c88"

FAIL_COUNT=0
TEST_COUNT=0
# Kept out of TEST_COUNT: a skipped check is coverage that did not happen,
# so it is neither a pass nor a failure.
SKIP_COUNT=0
SKIP_NOTE=""

CURL_STATUS=""
CURL_SIZE=""
CURL_BODY=""
CURL_HEAD=""
CURL_BODY_FILE=$(mktemp -t "amuleapi_$(basename "$0" .sh)_body.XXXXXX")
CURL_HEAD_FILE=$(mktemp -t "amuleapi_$(basename "$0" .sh)_head.XXXXXX")

# Phases register their cleanup here: a `trap ... EXIT` of their own would
# replace this one and leak the files above. A later call replaces the hook.
EXIT_HOOK=""
_on_exit() { EXIT_HOOK=$1; }
_lib_exit() {
	eval "$EXIT_HOOK"
	rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE"
}
trap _lib_exit EXIT

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
	local resp auth=()
	[ "${CURL_AUTO_AUTH:-0}" = 1 ] && auth=("${AUTH[@]}")
	# curl leaves -o alone when a response has no body, so without this a
	# 304 would be checked against the previous response.
	: > "$CURL_BODY_FILE"
	: > "$CURL_HEAD_FILE"
	if ! resp=$(curl -s --max-time "${CURL_MAX_TIME:-10}" \
		-D "$CURL_HEAD_FILE" -o "$CURL_BODY_FILE" \
		-w '%{http_code} %{size_download}' ${auth[@]+"${auth[@]}"} "$@"); then
		[ "${CURL_NONFATAL:-0}" = 1 ] || _die "curl invocation failed for $*"
		CURL_STATUS=000 CURL_SIZE=0 CURL_BODY="" CURL_HEAD=""
		return 0
	fi
	CURL_STATUS=${resp%% *}
	CURL_SIZE=${resp##* }
	# File bodies hold NUL bytes, which $(...) drops with a warning. Byte
	# checks read CURL_BODY_FILE.
	CURL_BODY=$(tr -d '\0' < "$CURL_BODY_FILE")
	CURL_HEAD=$(cat "$CURL_HEAD_FILE")
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
		|| { _fail "$label" "body was not valid JSON" "body: $CURL_BODY"; return; }
	if [ "$actual" = "$expected" ]; then
		_pass "$label"
	else
		_fail "$label" "expected $expected, got $actual" "body: $CURL_BODY"
	fi
}

_assert_body_empty() {
	local label=$1
	if [ -z "$CURL_BODY" ]; then
		_pass "$label"
	else
		_fail "$label" "expected an empty body, got: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

_assert_header_contains() {
	local needle=$1 label=$2
	if printf '%s' "$CURL_HEAD" | grep -qi -- "$needle"; then
		_pass "$label"
	else
		_fail "$label" "needle '$needle' not in response headers" \
			"headers: $(printf '%s' "$CURL_HEAD" | head -c 400)"
	fi
}

_assert_eq() {
	local expected=$1 actual=$2 label=$3
	if [ "$expected" = "$actual" ]; then
		_pass "$label"
	else
		_fail "$label" "expected [$expected], got [$actual]"
	fi
}

# Value of response header $1, named in lowercase, from the last _curl.
_header() { echo "$CURL_HEAD" | awk -F': ' -v k="$1" 'tolower($1) == k {gsub(/\r/,""); print $2}'; }

_jq() { printf '%s' "$CURL_BODY" | jq -r "$1" 2>/dev/null; }

# Prints the bearer token for password $1, or nothing when the login fails.
# $2 overrides the API root, for phases that start a second amuleapi.
_login() {
	curl -s -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$1\"}" "${2:-$API}/auth/login?include_token=true" \
		| jq -r '.token // empty'
}

_preflight() {
	command -v jq >/dev/null 2>&1 || _die "jq is required. brew install jq."
	curl -s -o /dev/null --max-time 2 "$API/health" 2>/dev/null \
		|| _die "amuleapi at $API is not reachable. Start amuleapi first."
}

# Set SKIP_NOTE first to replace the default skip suffix.
_summary() {
	local note=$SKIP_NOTE
	if [ -z "$note" ] && [ "$SKIP_COUNT" -gt 0 ]; then
		note=" ($SKIP_COUNT check(s) skipped)"
	fi
	echo
	if [ "$FAIL_COUNT" -eq 0 ]; then
		echo "OK: $TEST_COUNT/$TEST_COUNT passed$note"
		exit 0
	fi
	echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
	exit 1
}
