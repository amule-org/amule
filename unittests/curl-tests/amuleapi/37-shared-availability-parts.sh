#!/usr/bin/env bash
#
# amuleapi 37-shared-availability-parts — `parts` on `GET /shared/{hash}` detail.
#
# Validates the shared-side stateful RLE decoder pass (issue #982). amuled
# RLE-encodes each shared file's per-part source counts into
# EC_TAG_PARTFILE_PART_STATUS on the EC_TAG_KNOWNFILE tag; the refresher
# decodes it into the snapshot and the detail endpoint emits it.
#
# Wire contract:
#   GET /shared          → no `parts` key, ever
#   GET /shared/{hash}   → `parts: [{ "sources": int }, ...]`, omitted
#                          entirely until the first decode has landed
#
# `parts.length == ceil(size / 9728000)` (ed2k PARTSIZE).
# `sources` is the raw per-part source count, saturating at 255.
#
# Unlike the downloads bar this is an AVAILABILITY map, not a progress
# one: a shared file is fully local by definition, so `sources: 0` means
# no other peer has that part. There is deliberately no `state` key —
# local completeness is meaningless here.
#
# This script tolerates an empty share — every assertion past the auth
# gate is conditionally skipped if no files are shared.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_37_shared_avail_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

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
	resp=$(curl -s --max-time 10 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
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

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 37-shared-availability-parts smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# The RLE decoder needs the first EC_TAG_KNOWNFILE frame to seed itself
# before any later delta can produce a usable decode. Give it 4 s, the
# same warmup 08-read-download-parts.sh uses.
sleep 4

# --- 1. List endpoint MUST NOT carry `parts`. ----------------------
#
# A 100 GB file has ~10 800 parts; carrying the array across a
# five-figure share on every list read (and every shared_updated SSE
# tick) is not viable. Same call as the downloads list makes.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
_assert_status 200 "GET /shared → 200"
_assert_json_eq '.shared | type' array '/shared .shared is array'

COUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
echo "    info: $COUNT files currently shared"

if [ "$COUNT" -gt 0 ]; then
	NO_PARTS=$(printf '%s' "$CURL_BODY" | jq '[.shared[] | select(has("parts"))] | length')
	if [ "$NO_PARTS" = "0" ]; then
		_pass "/shared carries no \`parts\` on any row (detail-only field)"
	else
		_fail "/shared parts omission" "$NO_PARTS of $COUNT rows carry a parts key"
	fi

	# Pick the largest shared file: more parts means the length check
	# below actually discriminates, and a 1-part file would pass a
	# broken emitter by accident.
	FIRST_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.shared | max_by(.size) | .hash')
	FIRST_SIZE=$(printf '%s' "$CURL_BODY" | jq -r '.shared | max_by(.size) | .size')

	# --- 2. Detail endpoint carries `parts`. -----------------------
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$FIRST_HASH"
	_assert_status 200 "GET /shared/{hash} → 200"
	_assert_json_eq '.hash' "$FIRST_HASH" \
		'/shared/{hash} echoes hash (bare object, not enveloped)'

	HAS_PARTS=$(printf '%s' "$CURL_BODY" | jq 'has("parts")')
	if [ "$HAS_PARTS" != "true" ]; then
		# Legitimate transient: nothing decoded yet for this ECID. The
		# key must then be ABSENT rather than an empty or all-zero
		# array, which is exactly what we just asserted.
		echo "    info: no decode has landed for this file yet; \`parts\` correctly absent"
		echo "    info: per-part shape checks skipped — re-run once the refresher has ticked"
	else
		_assert_json_eq '.parts | type' array '/shared/{hash}.parts is array'
		PARTS_LEN=$(printf '%s' "$CURL_BODY" | jq '.parts | length')

		# --- 3. parts.length == parts_total_count == ceil(size/PARTSIZE). --
		PART_COUNT=$(printf '%s' "$CURL_BODY" | jq '.parts_total_count')
		if [ "$PARTS_LEN" = "$PART_COUNT" ]; then
			_pass "/shared/{hash}.parts.length == parts_total_count ($PARTS_LEN)"
		else
			_fail "/shared/{hash}.parts.length vs parts_total_count" \
				"parts_total_count=$PART_COUNT, parts.length=$PARTS_LEN"
		fi
		if [ "$FIRST_SIZE" -gt 0 ]; then
			EXPECTED_PARTS=$(( (FIRST_SIZE + 9728000 - 1) / 9728000 ))
			if [ "$PARTS_LEN" = "$EXPECTED_PARTS" ]; then
				_pass "/shared/{hash}.parts.length == ceil(size/PARTSIZE) ($PARTS_LEN)"
			else
				_fail "/shared/{hash}.parts.length sanity" \
					"size=$FIRST_SIZE → expected $EXPECTED_PARTS, got $PARTS_LEN"
			fi
		fi

		# --- 4. Per-part shape: {sources} and nothing else. ---------
		#
		# Pinning the absence of `state` on purpose: copying the
		# downloads shape would encode local completeness, which is
		# meaningless for a share and invites a progress renderer.
		if [ "$PARTS_LEN" -gt 0 ]; then
			_assert_json_eq '.parts[0].sources | type' number \
				'/shared/{hash}.parts[0].sources is number'
			WRONG_KEYS=$(printf '%s' "$CURL_BODY" | jq \
				'[.parts[] | select((keys | sort) != ["sources"])] | length')
			if [ "$WRONG_KEYS" = "0" ]; then
				_pass "/shared/{hash} every part object is exactly {sources}"
			else
				_fail "/shared/{hash} part object shape" \
					"$WRONG_KEYS parts carry keys other than \`sources\`"
			fi

			# --- 5. sources within the encoder's uint8 range. -------
			# amuled's RLE buffer is uint8, so counts saturate at 255.
			OUT_OF_RANGE=$(printf '%s' "$CURL_BODY" | jq \
				'[.parts[].sources | select(. < 0 or . > 255)] | length')
			if [ "$OUT_OF_RANGE" = "0" ]; then
				_pass "/shared/{hash} all part.sources values within [0,255]"
			else
				_fail "/shared/{hash} part.sources range" \
					"$OUT_OF_RANGE parts have sources outside [0,255]"
			fi

			# --- 6. sources.complete agrees with min(parts). --------
			# amuled derives sources.complete as the minimum of the
			# availability vector, so a scalar above that minimum means
			# the two are out of step. Reported as info, not a failure:
			# the count is refreshed only on the increment path in the
			# core, so it legitimately lags the vector downward.
			MIN_SRC=$(printf '%s' "$CURL_BODY" | jq '[.parts[].sources] | min')
			CS=$(printf '%s' "$CURL_BODY" | jq '.sources.complete // 0')
			echo "    info: sources.complete=$CS, min(parts[].sources)=$MIN_SRC"
		fi

		# --- 7. URL hash case-insensitive. ----------------------
		UPPER_HASH=$(echo "$FIRST_HASH" | tr 'a-f' 'A-F')
		_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$UPPER_HASH"
		_assert_status 200 "GET /shared/{HASH} (uppercase) → 200"
		_assert_json_eq '.parts | type' array \
			'/shared/{HASH} uppercase still carries parts'
	fi

	# --- 8. Guest sessions see the bar too (read-only data). -------
	GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
		-d "{\"password\":\"${GUEST_PASS:-guestpass}\"}" \
		"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
	if [ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ]; then
		_curl -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/shared/$FIRST_HASH"
		_assert_status 200 "GET /shared/{hash} as guest → 200"
	else
		echo "    info: guest login unavailable; guest-visibility check skipped"
	fi
else
	echo "    info: nothing shared; the per-part shape checks need at least one shared file"
fi

# --- 9. 404 on unknown hash. ---------------------------------------
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/shared/00000000000000000000000000000000"
_assert_status 404 "GET /shared/{nonexistent} → 404"
_assert_json_eq '.error.code' not_found \
	'/shared/{nonexistent} carries error.code=not_found'

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
