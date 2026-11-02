#!/usr/bin/env bash
#
# amuleapi 39-shared-media-refresh — re-extracting media metadata (issue #1079).
#
# Wire contract:
#   POST /shared/media/refresh          → 202 { scope:"all",  queued_file_count }
#   POST /shared/{hash}/media/refresh   → 202 { scope:"file", queued_file_count }
#
# `queued_file_count` counts files ACCEPTED for probing, not files that produced
# metadata: the scheduler drops anything that is not audio/video by
# extension, is an incomplete download, or is missing on disk. Nothing has
# been extracted when the response returns -- the probes run on amuled's
# media-probe worker.
#
# Tolerates an empty share, a daemon that predates the operation
# (503 ec_unsupported), and a daemon without its media helper, which refuses
# with 400 amuled_rejected (files.media_metadata_available is false).

set -u
set -o pipefail

CURL_MAX_TIME=15
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

_preflight

echo "amuleapi 39-shared-media-refresh smoke @ $HOST"

TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] || _die "login failed"

_curl -H "Authorization: Bearer $TOKEN" "$API/preferences"
AVAILABLE=$(printf '%s' "$CURL_BODY" | jq -r '.files.media_metadata_available')
echo "    info: media helper available: $AVAILABLE"

# --- 1. Whole-share refresh. ---------------------------------------
_curl -X POST -H "Authorization: Bearer $TOKEN" "$API/shared/media/refresh"
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
if [ "$AVAILABLE" = "false" ]; then
	_assert_status 400 "POST /shared/media/refresh without a media helper → 400"
	_assert_json_eq '.error.code' amuled_rejected 'missing helper carries error.code=amuled_rejected'
else
	_assert_status 202 "POST /shared/media/refresh → 202 Accepted"
	# `scope` and the count stay: it is a real count of what the scheduler
	# accepted, which no later read reports. There is no `ok` field; the 202 carries it.
	_assert_json_eq '. | has("ok")' false 'whole-share refresh has no constant ok field'
	_assert_json_eq '.scope'         all   'whole-share refresh reports scope=all'
	_assert_json_eq '.queued_file_count | type' number 'queued_file_count is numeric'
fi

# --- 2. Method + auth gating. --------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$API/shared/media/refresh"
_assert_status 405 "GET /shared/media/refresh → 405"
_curl -X POST "$API/shared/media/refresh"
_assert_status 401 "POST without a token → 401"

GUEST_TOKEN=$(_login "$GUEST_PASS")
if [ -n "$GUEST_TOKEN" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" "$API/shared/media/refresh"
	_assert_status 403 "POST as guest → 403 (refresh is admin-only)"
else
	echo "    info: guest login unavailable; guest check skipped"
fi

# --- 3. Unknown hash. ----------------------------------------------
_curl -X POST -H "Authorization: Bearer $TOKEN" \
	"$API/shared/00000000000000000000000000000000/media/refresh"
_assert_status 404 "POST /shared/{unknown}/media/refresh → 404"
_assert_json_eq '.error.code' not_found 'unknown hash carries error.code=not_found'

# --- 4. Single-file refresh, when there is a file to refresh. ------
_curl -H "Authorization: Bearer $TOKEN" "$API/shared"
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
	_curl -H "Authorization: Bearer $TOKEN" "$API/shared/$CANDIDATE"
	FTYPE=$(printf '%s' "$CURL_BODY" | jq -r '.file_type // empty')
	if [ "$FTYPE" = "audio" ] || [ "$FTYPE" = "video" ]; then
		HASH=$CANDIDATE
		echo "    info: probing $FTYPE file $HASH"
		break
	fi
done
if [ -n "$HASH" ]; then
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		"$API/shared/$HASH/media/refresh"
	# 409 is a legitimate answer for an incomplete download; both shapes are
	# contract, so accept either and check the one that came back.
	if [ "$CURL_STATUS" = "409" ]; then
		_assert_json_eq '.error.code' partfile_unsupported \
			'incomplete download → 409 partfile_unsupported'
	elif [ "$AVAILABLE" = "false" ]; then
		_assert_status 400 "POST /shared/{hash}/media/refresh without a media helper → 400"
		_assert_json_eq '.error.code' amuled_rejected \
			'missing helper carries error.code=amuled_rejected'
	else
		_assert_status 202 "POST /shared/{hash}/media/refresh → 202"
		_assert_json_eq '. | has("ok")' false \
			'single-file refresh has no constant ok field'
		_assert_json_eq '.scope' file 'single-file refresh reports scope=file'
	fi

	UPPER=$(echo "$HASH" | tr 'a-f' 'A-F')
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		"$API/shared/$UPPER/media/refresh"
	# A missing helper is refused only after the hash resolved, so 400 counts.
	if [ "$CURL_STATUS" = "202" ] || [ "$CURL_STATUS" = "409" ] ||
		{ [ "$AVAILABLE" = "false" ] && [ "$CURL_STATUS" = "400" ]; }; then
		_pass "uppercase hash is accepted (HTTP $CURL_STATUS)"
	else
		_fail "uppercase hash" "expected 202 or 409, got $CURL_STATUS"
	fi
else
	echo "    info: no audio/video file shared; single-file checks skipped"
fi

_summary
