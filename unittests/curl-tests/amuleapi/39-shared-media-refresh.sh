#!/usr/bin/env bash
#
# amuleapi 39-shared-media-refresh — re-extracting media metadata (issue #1079).
#
# Wire contract:
#   POST /shared/media/refresh          → 202 { ok, scope:"all",  queued }
#   POST /shared/{hash}/media/refresh   → 202 { ok, scope:"file", queued }
#
# `queued` counts files ACCEPTED for probing, not files that produced
# metadata: the scheduler drops anything that is not audio/video by
# extension, is an incomplete download, or is missing on disk. Nothing has
# been extracted when the response returns -- the probes run on amuled's
# media-probe worker.
#
# Tolerates an empty share and a daemon that predates the operation (501).

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_39_media_refresh_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"; shift
	for arg in "$@"; do echo "        $arg"; done
}
_curl() {
	local resp
	resp=$(curl -s --max-time 15 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
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
_assert_json_eq() {
	local expr=$1 expected=$2 label=$3 actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null) \
		|| _fail "$label" "body was not valid JSON" "body: $CURL_BODY"
	if [ "$actual" = "$expected" ]; then _pass "$label"; else
		_fail "$label" "expected $expected, got $actual" "body: $CURL_BODY"
	fi
}

command -v jq >/dev/null 2>&1 || _die "jq is required."
curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null \
	|| _die "amuleapi at $HOST is not reachable."

echo "amuleapi 39-shared-media-refresh smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# --- 1. Whole-share refresh. ---------------------------------------
_curl -X POST -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/media/refresh"
# 503 covers two different situations and only one of them is a skip:
# ec_unsupported is an amuled that predates the op, ec_unavailable is no
# usable EC link at all -- which is a broken rig, not a reason to report
# success. Branch on error.code, not on the status alone.
if [ "$CURL_STATUS" = "503" ]; then
	EC_CODE=$(printf '%s' "$CURL_BODY" | jq -r '.error.code // empty')
	if [ "$EC_CODE" = "ec_unavailable" ]; then
		_die "no usable EC link to amuled (503 ec_unavailable) -- rig is broken, not a skip"
	fi
	# A real assertion, not a _pass: any third error.code has to register as a
	# failure, which is also what keeps the FAIL_COUNT consult below live.
	_assert_json_eq '.error.code' ec_unsupported \
		'503 carries error.code=ec_unsupported (daemon predates the op)'
	echo "    info: connected amuled does not implement media refresh; rest skipped"
	echo
	# Consult FAIL_COUNT: the assertion above can fail, and printing OK over
	# a failed assertion is how a broken gate looks green.
	if [ "$FAIL_COUNT" -eq 0 ]; then
		echo "OK: $TEST_COUNT/$TEST_COUNT passed"
		exit 0
	fi
	echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
	exit 1
fi
_assert_status 202 "POST /shared/media/refresh → 202 Accepted"
_assert_json_eq '.ok'            true  'whole-share refresh reports ok'
_assert_json_eq '.scope'         all   'whole-share refresh reports scope=all'
_assert_json_eq '.queued | type' number 'queued is numeric'

# --- 2. Method + auth gating. --------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/media/refresh"
_assert_status 405 "GET /shared/media/refresh → 405"
_curl -X POST "$HOST/api/v0/shared/media/refresh"
_assert_status 401 "POST without a token → 401"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"${GUEST_PASS}\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
if [ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/shared/media/refresh"
	_assert_status 403 "POST as guest → 403 (refresh is admin-only)"
else
	echo "    info: guest login unavailable; guest check skipped"
fi

# --- 3. Unknown hash. ----------------------------------------------
_curl -X POST -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/shared/00000000000000000000000000000000/media/refresh"
_assert_status 404 "POST /shared/{unknown}/media/refresh → 404"
_assert_json_eq '.error.code' not_found 'unknown hash carries error.code=not_found'

# --- 4. Single-file refresh, when there is a file to refresh. ------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
echo "    info: $COUNT files currently shared"
# Pick an audio/video entry rather than shared[0]: a share whose first file is
# a document or an archive answers 400 (not eligible), which is correct daemon
# behaviour and would fail the phase for the wrong reason.
#
# file_type lives on the detail endpoint, not on the list, so this walks the
# hashes and asks. Capped at 20 to keep the probe bounded on a large share --
# if the first 20 are all documents the phase skips rather than misreporting.
HASH=
for CANDIDATE in $(printf '%s' "$CURL_BODY" | jq -r '.shared[0:20][].hash'); do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$CANDIDATE"
	FTYPE=$(printf '%s' "$CURL_BODY" | jq -r '.file_type // empty')
	if [ "$FTYPE" = "audio" ] || [ "$FTYPE" = "videos" ]; then
		HASH=$CANDIDATE
		echo "    info: probing $FTYPE file $HASH"
		break
	fi
done
if [ -n "$HASH" ]; then
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/shared/$HASH/media/refresh"
	# 409 is a legitimate answer for an incomplete download; both shapes are
	# contract, so accept either and check the one that came back.
	if [ "$CURL_STATUS" = "409" ]; then
		_assert_json_eq '.error.code' partfile_unsupported \
			'incomplete download → 409 partfile_unsupported'
	else
		_assert_status 202 "POST /shared/{hash}/media/refresh → 202"
		_assert_json_eq '.ok'    true 'single-file refresh reports ok'
		_assert_json_eq '.scope' file 'single-file refresh reports scope=file'
	fi

	UPPER=$(echo "$HASH" | tr 'a-f' 'A-F')
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/shared/$UPPER/media/refresh"
	if [ "$CURL_STATUS" = "202" ] || [ "$CURL_STATUS" = "409" ]; then
		_pass "uppercase hash is accepted (HTTP $CURL_STATUS)"
	else
		_fail "uppercase hash" "expected 202 or 409, got $CURL_STATUS"
	fi
else
	echo "    info: no audio/video file shared; single-file checks skipped"
fi

echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
