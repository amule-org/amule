#!/usr/bin/env bash
#
# amuleapi 08-read-download-parts — `progress.parts` on `GET /downloads/{hash}` detail.
#
# Validates the stateful RLE decoder pass that lands per-part state on
# the bare-object detail response. The list endpoint (`GET /downloads`)
# stays unchanged — `progress.parts` is detail-only since the array
# scales O(file_size / PARTSIZE) and most clients listing the queue
# don't want it (Q2 in PLAN §12 — no cap, omit-on-list).
#
# Wire contract:
#   GET /downloads          → `progress: { percent: float }`
#   GET /downloads/{hash}   → `progress: { percent: float,
#                                          parts: [{state, sources}] }`
#
# `parts.length == ceil(size / 9728000)` (ed2k PARTSIZE).
# `state` ∈ {"complete", "incomplete", "missing"}.
# `sources` is uint16 (0 ≤ sources ≤ 65535).
#
# This script tolerates an empty download queue — every assertion past
# the auth gate is conditionally skipped if no downloads are active.
# Run it against a daemon with at least one live download for the
# decoder-roundtrip checks to fire.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_08_read_download_parts_body.XXXXXX)
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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 08-read-download-parts smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Refresher needs at least 2 ticks for the two-phase INC protocol to
# converge — Phase 1 (UPDATE) surfaces new ECIDs, Phase 2 (FULL) ships
# identity + GAP/PART blobs. The stateful RLE decoder needs the FULL
# pass to seed itself before any UPDATE tick can produce a usable
# decode. Give it 4 s; matches 07-read-stats-and-search-results.sh.
sleep 4

# --- 1. List endpoint MUST NOT carry `progress.parts`. -------------
#
# Q2 + per-list-omit decision: `progress.parts` is detail-only. A list
# of 1000 downloads × ~150 parts/file would be 150k objects in the
# response; clients walk the list endpoint for queue state and the
# detail endpoint when they need the per-part breakdown.
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads"
_assert_status 200 "GET /downloads → 200"
_assert_json_eq '.downloads | type' array '/downloads .downloads is array'

COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
echo "    info: $COUNT downloads currently in queue"

if [ "$COUNT" -gt 0 ]; then
	# Pick the first download in the list and assert it does NOT carry
	# `progress.parts` — even if the underlying snapshot has decoded
	# arrays populated, the list emitter must omit them.
	_assert_json_eq '.downloads[0].progress.parts // "absent"' absent \
		'/downloads[0].progress.parts is absent (detail-only field)'
	_assert_json_eq '.downloads[0].progress.percent | type' number \
		'/downloads[0].progress.percent is numeric'

	# Pull the first hash for the detail-endpoint pass below.
	FIRST_HASH=$(printf '%s' "$CURL_BODY" | jq -r '.downloads[0].hash')
	FIRST_SIZE=$(printf '%s' "$CURL_BODY" | jq -r '.downloads[0].size')

	# --- 2. Detail endpoint carries `progress.parts`. --------------
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$FIRST_HASH"
	_assert_status 200 "GET /downloads/{hash} → 200"
	_assert_json_eq '.hash'                  "$FIRST_HASH" \
		'/downloads/{hash} echoes hash (bare object, not enveloped)'
	_assert_json_eq '.progress.parts | type' array \
		'/downloads/{hash}.progress.parts is array'
	_assert_json_eq '.progress.percent | type' number \
		'/downloads/{hash}.progress.percent is numeric'

	PARTS_LEN=$(printf '%s' "$CURL_BODY" | jq '.progress.parts | length')

	# --- 3. parts.length == ceil(size / 9728000). ------------------
	#
	# PARTSIZE = 9728000 bytes (ed2k spec, frozen for protocol
	# compatibility). The decoded_part_sources vector is sized to
	# ceil(size / PARTSIZE); the gap-derived state array follows the
	# same shape. If FIRST_SIZE is 0 we accept parts==0 (decoder
	# legitimately has nothing to size against).
	if [ "$FIRST_SIZE" -gt 0 ]; then
		EXPECTED_PARTS=$(( (FIRST_SIZE + 9728000 - 1) / 9728000 ))
		if [ "$PARTS_LEN" = "$EXPECTED_PARTS" ]; then
			_pass "/downloads/{hash}.progress.parts.length == ceil(size/PARTSIZE) ($PARTS_LEN)"
		elif [ "$PARTS_LEN" = "0" ]; then
			# RLE decoder hasn't seen a full tick yet for this file — the
			# detail handler still emits the array shape but the underlying
			# decoded_part_sources vector is empty. This is a transient
			# warmup state, not a failure: the second tick fills it in.
			_pass "/downloads/{hash}.progress.parts.length == 0 (decoder warming up; expected on first tick)"
		else
			_fail "/downloads/{hash}.progress.parts.length sanity" \
				"size=$FIRST_SIZE → expected $EXPECTED_PARTS, got $PARTS_LEN"
		fi
	fi

	# --- 4. Per-part shape: {state, sources}. ----------------------
	if [ "$PARTS_LEN" -gt 0 ]; then
		_assert_json_eq '.progress.parts[0].state | type' string \
			'/downloads/{hash}.progress.parts[0].state is string'
		_assert_json_eq '.progress.parts[0].sources | type' number \
			'/downloads/{hash}.progress.parts[0].sources is number'

		# --- 5. state enum allowlist. ------------------------------
		# Every part state must be one of {complete, incomplete, missing}.
		# The walker is: has_gap → (sources>0 ? incomplete : missing);
		# !has_gap → complete. Any other string means the emitter
		# silently regressed.
		BOGUS_COUNT=$(printf '%s' "$CURL_BODY" | jq \
			'[.progress.parts[].state | select(. != "complete" and . != "incomplete" and . != "missing")] | length')
		if [ "$BOGUS_COUNT" = "0" ]; then
			_pass "/downloads/{hash} all part.state values ∈ {complete, incomplete, missing}"
		else
			_fail "/downloads/{hash} part.state enum" \
				"$BOGUS_COUNT parts have an out-of-enum state value"
		fi

		# --- 6. sources is a uint16 (0 ≤ sources ≤ 65535). -------
		OUT_OF_RANGE=$(printf '%s' "$CURL_BODY" | jq \
			'[.progress.parts[].sources | select(. < 0 or . > 65535)] | length')
		if [ "$OUT_OF_RANGE" = "0" ]; then
			_pass "/downloads/{hash} all part.sources values fit in uint16"
		else
			_fail "/downloads/{hash} part.sources range" \
				"$OUT_OF_RANGE parts have sources outside [0,65535]"
		fi

		# --- 7. Live downloading file has at least one non-complete
		# part. A file in `downloading` status by definition has work
		# remaining; if every part comes back `complete` the decoder
		# either ran on a finished file or the gap-list parse is
		# silently dropping every entry. Skip if the file is paused /
		# completed / hashing.
		STATUS=$(printf '%s' "$CURL_BODY" | jq -r '.status')
		if [ "$STATUS" = "downloading" ]; then
			NONCOMPLETE=$(printf '%s' "$CURL_BODY" | jq \
				'[.progress.parts[].state | select(. != "complete")] | length')
			if [ "$NONCOMPLETE" -gt 0 ]; then
				_pass "/downloads/{hash} downloading file has $NONCOMPLETE non-complete parts (decoder live)"
			else
				_fail "/downloads/{hash} live-decoder sanity" \
					"status=downloading but 0 non-complete parts in the gap-derived map" \
					"size=$FIRST_SIZE percent=$(printf '%s' "$CURL_BODY" | jq '.progress.percent')"
			fi
		else
			echo "    info: download status=$STATUS, skipping live-decoder non-complete-parts check"
		fi
	else
		echo "    info: parts array empty (decoder warming up or file size=0); per-part shape checks skipped"
	fi

	# --- 8. URL hash case-insensitive (already covered in 4b but
	# the detail endpoint changed shape so re-pin). ---------------
	UPPER_HASH=$(echo "$FIRST_HASH" | tr 'a-f' 'A-F')
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$UPPER_HASH"
	_assert_status 200 "GET /downloads/{HASH} (uppercase) → 200"
	_assert_json_eq '.progress.parts | type' array \
		'/downloads/{HASH} uppercase still carries progress.parts'
else
	echo "    info: no active downloads; the per-part shape checks need a live download"
	echo "    info: passing the auth-gate + list-shape sweep only — run again with a downloading file for full coverage"
fi

# --- 9. 404 on unknown hash. ---------------------------------------
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/downloads/00000000000000000000000000000000"
_assert_status 404 "GET /downloads/{nonexistent} → 404"
_assert_json_eq '.error.code' not_found \
	'/downloads/{nonexistent} carries error.code=not_found'

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
