#!/usr/bin/env bash
#
# amuleapi /downloads, /downloads/{hash}, /shared. Exercises the
# consolidated GET_UPDATE @ EC_DETAIL_INC_UPDATE polling path end-
# to-end, the ECID-keyed state cache, the auth gate, and the bare-
# object detail shape.
#
# This smoke is intentionally tolerant of empty caches — it asserts
# the envelope shape and the per-item field types without requiring
# a specific download / upload / shared file to exist on the daemon.
# Field-content correctness is exercised by the unit tests against
# crafted EC packets, and by the live test the dev runs against
# their daemon (`./build-macos/src/webapi/amuleapi ...` →
# `curl /downloads | jq`).
#
# Bring-up convention:
#   rm -rf /tmp/amuleapi-04-read-downloads-shared && mkdir -p /tmp/amuleapi-04-read-downloads-shared
#   amuleapi --config-dir=/tmp/amuleapi-04-read-downloads-shared --host=127.0.0.1 \
#            --port=4712 --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-04-read-downloads-shared --host=127.0.0.1 \
#            --port=4712 --password=amule &
#   ./04-read-downloads-shared.sh

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_04_read_downloads_shared_body.XXXXXX)
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
	resp=$(curl -s --max-time 10 \
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
	_die "jq is required. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 04-read-downloads-shared smoke @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for phase 4b tests"

# Allow the first two refresher ticks to populate the cache (cold
# start: Phase 1 surfaces every existing file as "new", Phase 2 ships
# their identities; from tick 2 the cache is fully built).
sleep 3

# --- 1. Each list endpoint pre-auth → 401. -------------------------
for ep in downloads shared; do
	_curl "$HOST/api/v0/$ep"
	_assert_status 401 "GET /api/v0/$ep without creds → 401"
done

# --- 2. List endpoints with admin bearer → 200 + envelope shape. ---
for ep in downloads shared; do
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/$ep"
	_assert_status 200 "GET /api/v0/$ep (admin bearer) → 200"
	_assert_json_eq ".$ep | type"                array   "/$ep .$ep is an array"
done

# --- 3. /downloads element shape (only when there's at least one). -
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads"
COUNT=$(printf '%s' "$CURL_BODY" | jq '.downloads | length')
if [ "$COUNT" -gt 0 ]; then
	echo "  --- /downloads has $COUNT entry/entries; shape checks ---"
	_assert_json_eq '.downloads[0].hash | length' 32 \
		'/downloads[0].hash is 32-char hex'
	_assert_json_eq '.downloads[0].ecid | type' null \
		'/downloads[0] does not expose internal ecid'
	_assert_json_eq '.downloads[0].name | type' string \
		'/downloads[0].name is string'
	_assert_json_eq '.downloads[0].size | type' number \
		'/downloads[0].size is numeric'
	_assert_json_eq '.downloads[0].status | test("^(downloading|paused|completed|hashing|erroneous|completing|allocating|waiting|insufficient_disk|unknown)$")' \
		true '/downloads[0].status is a known enum value'
	_assert_json_eq '.downloads[0].priority | test("^(very_low|low|normal|high|release|auto)$")' \
		true '/downloads[0].priority is a known enum value'
	_assert_json_eq '.downloads[0].progress.percent | type' number \
		'/downloads[0].progress.percent is numeric'
	_assert_json_eq '.downloads[0].sources | type' object \
		'/downloads[0].sources is object'
	_assert_json_eq '.downloads[0].sources.total | type' number \
		'/downloads[0].sources.total is numeric'

	# --- 4. /downloads/{hash} bare-object detail. -----------------
	HASH=$(printf '%s' "$CURL_BODY" | jq -r '.downloads[0].hash')
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH"
	_assert_status 200 "GET /api/v0/downloads/{hash} → 200"
	# Detail response is bare — `hash` at top level, no `snapshot_at`
	# envelope (Q3 in PLAN.md §12).
	_assert_json_eq '.hash' "$HASH" '/downloads/{hash} returns bare object keyed by hash'
	_assert_json_eq '.snapshot_at | type' null \
		'/downloads/{hash} has no snapshot_at envelope (bare object)'
	_assert_json_eq '.progress.percent | type' number \
		'/downloads/{hash} carries progress.percent'
	# Part-A detail fields (issue #417) — detail-only, type-tolerant.
	_assert_json_eq '.part_count | type' number \
		'/downloads/{hash} carries part_count'
	_assert_json_eq '.remaining_time | type' number \
		'/downloads/{hash} carries remaining_time'
	_assert_json_eq '.aich_hash | type' string \
		'/downloads/{hash} carries aich_hash'
	_assert_json_eq '.met_file | type' string \
		'/downloads/{hash} carries met_file'
	_assert_json_eq '.queued_count | type' number \
		'/downloads/{hash} carries queued_count'
	_assert_json_eq '.comment | type' string \
		'/downloads/{hash} carries comment'
	_assert_json_eq '.rating | type' number \
		'/downloads/{hash} carries rating'
	_assert_json_eq '.a4af_auto | type' boolean \
		'/downloads/{hash} carries a4af_auto'

	# Per-source comments sub-resource (issue #419).
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/comments"
	_assert_status 200 "GET /downloads/{hash}/comments → 200"
	_assert_json_eq '.count | type' number \
		'/downloads/{hash}/comments carries numeric count'
	_assert_json_eq '.comments | type' array \
		'/downloads/{hash}/comments.comments is an array'

	# Source-reported filenames sub-resource (issue #420).
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/filenames"
	_assert_status 200 "GET /downloads/{hash}/filenames → 200"
	_assert_json_eq '.filenames | type' array \
		'/downloads/{hash}/filenames.filenames is an array'

	# A4AF source list sub-resource (issue #421).
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 200 "GET /downloads/{hash}/a4af → 200"
	_assert_json_eq '.a4af_auto | type' boolean \
		'/downloads/{hash}/a4af carries a4af_auto'
	_assert_json_eq '.sources | type' array \
		'/downloads/{hash}/a4af.sources is an array'

	# Unknown action → 400 (mutation validation; admin token).
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"bogus"}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 400 "POST /downloads/{hash}/a4af unknown action → 400"

	# Valid action → 200 (no-op on a download with no A4AF sources, but
	# exercises the EC op path). Response echoes the A4AF view.
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_others"}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 200 "POST /downloads/{hash}/a4af swap_others → 200"
	_assert_json_eq '.sources | type' array \
		'POST /a4af response carries sources array'

	# Uppercase hash → same hit (case-insensitive route).
	HASH_UPPER=$(echo "$HASH" | tr '[:lower:]' '[:upper:]')
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH_UPPER"
	_assert_status 200 "GET /downloads/{HASH-UPPERCASE} → 200 (case-insensitive)"
else
	echo "  --- /downloads is empty; skipping per-item shape + detail checks ---"
fi

# --- 5. Missing-hash 404. -----------------------------------------
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/downloads/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "GET /downloads/{nonexistent-hash} → 404"
_assert_json_eq '.error.code' not_found \
	'404 carries error.code=not_found'

# --- 6. /shared element shape (always at least .DS_Store on macOS). -
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
SHCOUNT=$(printf '%s' "$CURL_BODY" | jq '.shared | length')
if [ "$SHCOUNT" -gt 0 ]; then
	echo "  --- /shared has $SHCOUNT entry/entries; shape checks ---"
	_assert_json_eq '.shared[0].hash | length' 32 \
		'/shared[0].hash is 32-char hex'
	_assert_json_eq '.shared[0].ecid | type' null \
		'/shared[0] does not expose internal ecid'
	_assert_json_eq '.shared[0].xfer | type' object \
		'/shared[0].xfer is object'
	_assert_json_eq '.shared[0].xfer.total | type' number \
		'/shared[0].xfer.total is numeric'
	_assert_json_eq '.shared[0].priority | type' string \
		'/shared[0].priority is string'
	_assert_json_eq '.shared[0].priority_auto | type' boolean \
		'/shared[0].priority_auto is boolean'

	# --- 6b. GET /shared/{hash} detail endpoint (issue #417 Part B). ---
	SHASH=$(printf '%s' "$CURL_BODY" | jq -r '.shared[0].hash')
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH"
	_assert_status 200 "GET /api/v0/shared/{hash} → 200 (new detail endpoint)"
	_assert_json_eq '.hash' "$SHASH" \
		'/shared/{hash} returns bare object keyed by hash'
	_assert_json_eq '.snapshot_at | type' null \
		'/shared/{hash} has no snapshot_at envelope (bare object)'
	_assert_json_eq '.file_type | type' string \
		'/shared/{hash} carries file_type'
	_assert_json_eq '.share_ratio | type' number \
		'/shared/{hash} carries share_ratio'
	_assert_json_eq '.path | type' string \
		'/shared/{hash} carries path'
	_assert_json_eq '.complete_sources_range | type' object \
		'/shared/{hash} carries complete_sources_range'
	_assert_json_eq '.aich_hash | type' string \
		'/shared/{hash} carries aich_hash'
	_assert_json_eq '.part_count | type' number \
		'/shared/{hash} carries part_count'
	_assert_json_eq '.comment | type' string \
		'/shared/{hash} carries comment'
	_assert_json_eq '.rating | type' number \
		'/shared/{hash} carries rating'
fi

# --- 6c. /shared/{hash} missing-hash 404. -------------------------
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/shared/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "GET /shared/{nonexistent-hash} → 404"

# --- 7. Method gate. DELETE is method-gated on the /shared collection
# (no bulk-unshare endpoint); the /downloads collection now accepts a bulk
# DELETE (issue #358, exercised by 29-bulk-mutations.sh), so it is no
# longer 405 here.
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
_assert_status 405 "DELETE /api/v0/shared → 405"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
