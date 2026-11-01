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

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

_try_login() { # $1 = plaintext password
	_curl -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"$1\"}" "$API/auth/login"
}

_preflight

echo "amuleapi 02b-auth-lockout-isolation smoke @ $HOST"

# --- 0. Precondition: guest must actually be able to log in. -------
# The whole point is that a *guest* success cannot clear the bucket, so a
# daemon without a usable guest password proves nothing. Skip rather than
# fail: that is a bring-up shortfall, not a regression.
_try_login "$GUEST_PASS"
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
	_try_login "wrong-admin-guess-$i"
	_assert_status 401 "failed admin guess $i/4 → 401"
done

# --- 2. A successful GUEST login must not clear the admin streak. --
_try_login "$GUEST_PASS"
_assert_status 200 "guest login between the guesses → 200"

# --- 3. The streak survived: the 5th failure arms the lockout. -----
# Were the bucket cleared by step 2, this would be failure #1 of a fresh
# window and step 4 would answer 401 instead of 429.
_try_login "wrong-admin-guess-5"
_assert_status 401 "5th failed guess still returns 401 (arms the lockout)"

# --- 4. The next attempt is locked out. ----------------------------
# Asserted with the CORRECT admin password: a 429 here proves the lockout is
# in force rather than the password being wrong, which is the whole control.
_try_login "$ADMIN_PASS"
_assert_status 429 "correct admin password is locked out → 429"
_assert_json_eq '.error.code' rate_limited 'lockout carries error.code=rate_limited'

# --- 5. Guest cannot unlock it either. -----------------------------
# The bucket is per-IP, so a guest holder must not be able to log in and
# clear the lockout out from under the admin.
_try_login "$GUEST_PASS"
_assert_status 429 "guest is locked out by the same bucket → 429"

# --- Summary. -----------------------------------------------------
_summary
