#!/usr/bin/env bash
#
# amuleapi 30-shared-verify — POST /shared/{hash}/verify.
#
# Endpoint:
#   POST /api/v1/shared/{hash}/verify   → 202 {"ok":true}
#
# Re-hashes a shared file against its on-disk data (EC_OP_VERIFY_LOCAL_DATA).
# amuled queues a CVerifyLocalDataTask and answers immediately, so the call
# is accepted (202), never completed (200) — the verdict is only ever emitted
# as an amule log line ("Verify Local Data (...): Result OK" / "ERRORS
# FOUND!"), which a client reads back through /logs/amule or the SSE log
# channel. Section 6 reads it back there, on a file of its own.
#
# Partfiles are rejected with 409 partfile_unsupported: the hashing task
# bails out on IsPartFile(), so accepting one would promise a report that
# never arrives.

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

AMULE_SHARED_DIR=${AMULE_SHARED_DIR:-}

_preflight

echo "amuleapi 30-shared-verify smoke @ $HOST"

ADMIN_TOKEN=$(_login "$ADMIN_PASS")
[ -n "$ADMIN_TOKEN" ] || _die "admin login failed"

GUEST_TOKEN=$(_login "$GUEST_PASS")
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && HAVE_GUEST=1

sleep 4

# Verify only applies to completed knownfiles, so the fixture path is the
# same one 17-shared-priority-patch uses: plant a real file in a directory
# amuled shares and let it hash in. Reused across runs once it exists.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/shared"
COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')

if [ "$COUNT" = "0" ]; then
	[ -n "$AMULE_SHARED_DIR" ] \
		|| _die "set AMULE_SHARED_DIR to a directory amuled shares so the smoke can plant a fixture"
	FIXTURE="$AMULE_SHARED_DIR/amuleapi-regtest-shared.dat"
	if [ ! -f "$FIXTURE" ]; then
		head -c 1048576 /dev/urandom > "$FIXTURE" \
			|| _die "cannot write fixture to AMULE_SHARED_DIR=$AMULE_SHARED_DIR"
	fi
	echo "    info: planted fixture $FIXTURE; reloading shares"
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$API/shared_reload"
	for _ in $(seq 1 30); do
		sleep 1
		_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/shared"
		COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
		[ "$COUNT" != "0" ] && break
	done
	[ "$COUNT" != "0" ] \
		|| _die "fixture planted in $AMULE_SHARED_DIR but never appeared in /shared after reload"
fi

# Pick a completed entry — a shared partfile is rejected by design, so it
# can't stand in for the happy path. The list object carries no partfile
# flag, so classify on the detail object's `incomplete`. The same sweep
# picks up a partfile for the 409 guard below, when the library has one.
TEST_HASH=""
PART_HASH=""
for h in $(printf '%s' "$CURL_BODY" | jq -r '.shared[].hash' | head -20); do
	INCOMPLETE=$(curl -s --max-time 10 -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$API/shared/$h" | jq -r '.incomplete')
	if [ "$INCOMPLETE" = "true" ]; then
		[ -z "$PART_HASH" ] && PART_HASH=$h
	else
		[ -z "$TEST_HASH" ] && TEST_HASH=$h
	fi
	[ -n "$TEST_HASH" ] && [ -n "$PART_HASH" ] && break
done
[ -n "$TEST_HASH" ] || _die "no completed (non-partfile) shared file available to verify"
echo "    info: verifying hash=$TEST_HASH"

# --- 1. Auth + admin gate. -----------------------------------------
_curl -X POST "$API/shared/$TEST_HASH/verify"
_assert_status 401 "POST /shared/{hash}/verify (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		"$API/shared/$TEST_HASH/verify"
	_assert_status 403 "POST /shared/{hash}/verify (guest) → 403"
fi

# --- 2. Happy path: accepted, not completed. -----------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/shared/$TEST_HASH/verify"
# 202 with no body: the hash came from the URL, and the outcome only arrives
# later, on hashed_part_count and the log line.
_assert_status 202 "POST /shared/{hash}/verify → 202"
_assert_body_empty "verify sends no body"

# Uppercase hash resolves the same file (lookup lowercases the capture).
UPPER_HASH=$(printf '%s' "$TEST_HASH" | tr 'a-f' 'A-F')
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/shared/$UPPER_HASH/verify"
_assert_status 202 "POST /shared/{HASH}/verify (uppercase) → 202"

# --- 3. Unknown hash → 404. ----------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/shared/00000000000000000000000000000000/verify"
_assert_status 404 "POST /shared/{unknown}/verify → 404"
_assert_json_eq '.error.code' not_found "unknown hash → error.code=not_found"

# --- 4. Method gate. -----------------------------------------------
_curl -X GET -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/shared/$TEST_HASH/verify"
_assert_status 405 "GET /shared/{hash}/verify → 405"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" -d '{}' \
	"$API/shared/$TEST_HASH/verify"
_assert_status 405 "PATCH /shared/{hash}/verify → 405"

# --- 5. Partfile guard (only when the library has one). ------------
# A partfile shows up in /shared once ≥1 part has completed. Classified in
# the sweep above. Skipped rather than forced, since planting a
# half-finished download isn't reproducible in a smoke run.
if [ -n "$PART_HASH" ]; then
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		"$API/shared/$PART_HASH/verify"
	_assert_status 409 "POST /shared/{partfile}/verify → 409"
	_assert_json_eq '.error.code' partfile_unsupported \
		"partfile → error.code=partfile_unsupported"
else
	echo "    info: no shared partfile present; skipping the 409 guard check"
fi

# --- 6. Outcome in the amule log. --------------------------------------
# Uses a file of its own, because it changes it on disk. A size that no longer
# matches is a failed check, reported as ERRORS FOUND rather than skipped.
_log_count() {
	curl -s --max-time 10 -H "Authorization: Bearer $ADMIN_TOKEN" "$API/logs/amule?tail=500" \
		| jq --arg re "$1" '[.lines[] | select(test($re))] | length'
}

# _verify_logs <hash> <regex> <label>: verify, then wait for one more matching line.
_verify_logs() {
	local hash=$1 re=$2 label=$3 before now
	before=$(_log_count "$re")
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$API/shared/$hash/verify"
	for _ in $(seq 1 30); do
		now=$(_log_count "$re")
		[ "${now:-0}" -gt "${before:-0}" ] && break
		sleep 1
	done
	if [ "${now:-0}" -gt "${before:-0}" ]; then
		_pass "$label"
	else
		_fail "$label" "no new log line matching: $re"
	fi
}

if [ -n "$AMULE_SHARED_DIR" ]; then
	VNAME=amuleapi-regtest-verify-size.dat
	VFILE="$AMULE_SHARED_DIR/$VNAME"
	head -c 1048576 /dev/urandom > "$VFILE" || _die "cannot write $VFILE"
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$API/shared_reload"
	VHASH=""
	for _ in $(seq 1 30); do
		sleep 1
		VHASH=$(curl -s --max-time 10 -H "Authorization: Bearer $ADMIN_TOKEN" "$API/shared" \
			| jq -r --arg n "$VNAME" '.shared[] | select(.name == $n) | .hash')
		[ -n "$VHASH" ] && break
	done
	if [ -n "$VHASH" ]; then
		_verify_logs "$VHASH" "Verify Local Data \\(.*\\): Result OK for .*$VNAME" \
			"intact file → 'Result OK' log line"
		printf 'x' >> "$VFILE"
		_verify_logs "$VHASH" "Verify Local Data: ERRORS FOUND! .*$VNAME Size on disk" \
			"file grown on disk → 'ERRORS FOUND! ... Size on disk' log line"
	else
		_fail "verify outcome fixture" "$VNAME never appeared in /shared after reload"
	fi
	rm -f "$VFILE"
	curl -s -o /dev/null -X POST -H "Authorization: Bearer $ADMIN_TOKEN" "$API/shared_reload"
else
	echo "    info: AMULE_SHARED_DIR unset; skipping the outcome checks"
fi

# --- Summary. -----------------------------------------------------
_summary
