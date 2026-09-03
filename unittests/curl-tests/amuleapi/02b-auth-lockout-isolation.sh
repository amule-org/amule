#!/usr/bin/env bash
#
# amuleapi 02b-auth-lockout-isolation — the login lockout must not be
# clearable with a *different* credential.
#
# One bucket, keyed by IP, guards both passwords: VerifyPassword tries admin
# then guest. If a successful login cleared that bucket, a guest-credential
# holder could brute-force the admin password indefinitely -- four wrong admin
# guesses, one good guest login to wipe the streak, repeat -- and the one
# control protecting the admin password would never fire.
#
# Its own phase rather than a block in 02-auth.sh: proving the streak survived
# means crossing the threshold, which locks this IP out, and 02-auth's own
# rate-limit block counts attempts from a clean bucket.
#
# Bring-up convention (matches the README in the parent dir):
#   rm -rf /tmp/amuleapi-02b && mkdir -p /tmp/amuleapi-02b
#   amuleapi --config-dir=/tmp/amuleapi-02b --host=127.0.0.1 \
#            --port=4712 --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-02b --host=127.0.0.1 \
#            --port=4712 --password=amule --set-guest-pass=guestpass
#   amuleapi --config-dir=/tmp/amuleapi-02b --host=127.0.0.1 \
#            --port=4712 --password=amule &
#   ./02b-auth-lockout-isolation.sh
#
# Environment:
#   HOST=localhost:4713         amuleapi endpoint
#   ADMIN_PASS=adminpass        plaintext admin password
#   GUEST_PASS=guestpass        plaintext guest password
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0
SKIP_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_02b_body.XXXXXX)
CURL_HDR_FILE=$(mktemp -t amuleapi_02b_hdr.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HDR_FILE"' EXIT

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

_login() { # $1 = plaintext password
	_curl -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$1\"}" "$HOST/api/v0/auth/login"
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 02b-auth-lockout-isolation smoke @ $HOST"

# --- 0. Precondition: guest must actually be able to log in. -------
# The whole point is that a *guest* success cannot clear the bucket, so a
# daemon without a usable guest password proves nothing. Skip rather than
# fail: that is a bring-up shortfall, not a regression.
_login "$GUEST_PASS"
if [ "$CURL_STATUS" != "200" ]; then
	_skip "guest login unavailable (HTTP $CURL_STATUS) - cannot test cross-credential reset"
	echo
	echo "PASS: $((TEST_COUNT-FAIL_COUNT))/$TEST_COUNT passed ($SKIP_COUNT skipped)"
	exit 0
fi
_pass "precondition: guest can log in"

# --- 1. Four failed admin guesses, one below the threshold. --------
# Default limiter: 5 failures / 60 s → 300 s lockout.
for i in 1 2 3 4; do
	_login "wrong-admin-guess-$i"
	_assert_status 401 "failed admin guess $i/4 → 401"
done

# --- 2. A successful GUEST login must not clear the admin streak. --
_login "$GUEST_PASS"
_assert_status 200 "guest login between the guesses → 200"

# --- 3. The streak survived: the 5th failure arms the lockout. -----
# Were the bucket cleared by step 2, this would be failure #1 of a fresh
# window and step 4 would answer 401 instead of 429.
_login "wrong-admin-guess-5"
_assert_status 401 "5th failed guess still returns 401 (arms the lockout)"

# --- 4. The next attempt is locked out. ----------------------------
# Asserted with the CORRECT admin password: a 429 here proves the lockout is
# in force rather than the password being wrong, which is the whole control.
_login "$ADMIN_PASS"
_assert_status 429 "correct admin password is locked out → 429"
_assert_json_eq '.error.code' rate_limited 'lockout carries error.code=rate_limited'

# --- 5. Guest cannot unlock it either. -----------------------------
# The bucket is per-IP, so a guest holder must not be able to log in and
# clear the lockout out from under the admin.
_login "$GUEST_PASS"
_assert_status 429 "guest is locked out by the same bucket → 429"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed${SKIP_COUNT:+ ($SKIP_COUNT skipped)}"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
