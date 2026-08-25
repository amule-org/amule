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
# Skips are counted apart from TEST_COUNT, never folded into it: a skipped
# check is coverage that did not happen, and adding it to the passed tally
# would report the absence of a check as a check that succeeded.
SKIP_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_04_read_downloads_shared_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

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
	_assert_json_eq '.downloads[0].status | test("^(downloading|paused|stopped|completed|hashing|erroneous|completing|allocating|waiting|insufficient_disk|unknown)$")' \
		true '/downloads[0].status is a known enum value'
	_assert_json_eq '.downloads[0].priority | test("^(very_low|low|normal|high|release|auto)$")' \
		true '/downloads[0].priority is a known enum value'
	_assert_json_eq '.downloads[0].progress.percent | type' number \
		'/downloads[0].progress.percent is numeric'
	_assert_json_eq '.downloads[0].sources | type' object \
		'/downloads[0].sources is object'
	_assert_json_eq '.downloads[0].sources.total | type' number \
		'/downloads[0].sources.total is numeric'
	_assert_json_eq '.downloads[0].kad_comment_search_running | type' boolean \
		'/downloads[0].kad_comment_search_running is boolean (issue #434)'
	# Moved onto the list by issue #1054 — it used to be detail-only, which
	# left a list-driven client with no way to see a hash running.
	_assert_json_eq '.downloads[0].hashing_progress | type' number \
		'/downloads[0].hashing_progress is numeric (#1054)'

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
	# null when stalled/paused: nothing to compute an ETA from. It was -1,
	# which a client had to know meant "unknown".
	_assert_json_eq '(.remaining_time == null or (.remaining_time | type) == "number")' true \
		'/downloads/{hash} remaining_time is a number or null'
	_assert_json_eq '.aich_hash | type' string \
		'/downloads/{hash} carries aich_hash'
	_assert_json_eq '.met_file | type' string \
		'/downloads/{hash} carries met_file'
	_assert_json_eq '.path | type' string \
		'/downloads/{hash} carries path (#417)'
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
	_assert_json_eq '.kad_comment_search_running | type' boolean \
		'/downloads/{hash}/comments carries kad_comment_search_running flag'

	# Trigger an on-demand Kad notes lookup (issue #434). Async on the daemon;
	# 202 Accepted (or 400 amuled_rejected if Kad is not connected in the smoke
	# environment — accept either as a valid handled response, but not 404/405).
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/downloads/$HASH/comments"
	if [ "$CURL_STATUS" = "202" ] || [ "$CURL_STATUS" = "400" ]; then
		_pass "POST /downloads/{hash}/comments (Kad search) → $CURL_STATUS (accepted/handled)"
	else
		_fail "POST /downloads/{hash}/comments (Kad search)" \
			"expected 202 or 400, got $CURL_STATUS" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi

	# Source-reported filenames sub-resource (issue #420).
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/filenames"
	_assert_status 200 "GET /downloads/{hash}/filenames → 200"
	_assert_json_eq '.filenames | type' array \
		'/downloads/{hash}/filenames.filenames is an array'
	# `media` (issue #418) is omitted for a file with no probed metadata,
	# which the smoke daemon's test files never have.
	_assert_json_eq '.media | type' null \
		'/downloads/{hash} omits media when unprobed'

	# A4AF sources are the per-file client rows below, which carry the whole
	# peer object per source rather than a bare ECID, and `a4af_auto` lives on
	# the download detail object, asserted above. The a4af path is POST-only.

	# Unknown action → 400 (mutation validation; admin token).
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"bogus"}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 400 "POST /downloads/{hash}/a4af unknown action → 400"

	# Per-file client rows (issue #984): the peers of one file, with their
	# relation to it, replacing a client-side join against the global list.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients"
	_assert_status 200 "GET /downloads/{hash}/clients → 200"
	_assert_json_eq '.clients | type' array '/downloads/{hash}/clients returns a clients array'
	for k in total offset limit; do
		_assert_json_eq "has(\"$k\")" true "/downloads/{hash}/clients envelope has $k"
	done

	# Every row carries its relation to this file, and no row carries a parts
	# bitmap unless it was asked for. These are `[...] | length == 0` shapes,
	# which pass over an empty array whether or not the feature works, so
	# they are guarded on -- and the run prints -- the row count: a green run
	# must not claim coverage it did not get.
	DLROWS=$(printf '%s' "$CURL_BODY" | jq '.clients | length')
	echo "  --- /downloads/{hash}/clients returned $DLROWS row(s) ---"
	if [ "$DLROWS" -gt 0 ]; then
		_assert_json_eq '[.clients[] | select(.role as $r | ($r == null) or ((["source","peer","both","none"] | index($r)) == null))] | length' \
			0 "every row has a valid role"
		_assert_json_eq '[.clients[] | select(.a4af == null)] | length' 0 "every row has an a4af flag"
		_assert_json_eq '[.clients[] | select(has("parts"))] | length' 0 "no parts bitmap without include_parts"
	else
		_skip "row-shape checks: no peer is connected to the download"
	fi

	# Opt-in bitmaps are exactly part_count long for a row that has one.
	PARTCOUNT=$(curl -s -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH" | jq -r '.progress.parts | length')
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients?include_parts=true"
	_assert_status 200 "GET /downloads/{hash}/clients?include_parts=true → 200"
	DLBITMAPS=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(has("parts"))] | length')
	if [ "$DLBITMAPS" -gt 0 ]; then
		_assert_json_eq "[.clients[] | select(has(\"parts\")) | select((.parts | length) != $PARTCOUNT)] | length" \
			0 "every parts bitmap ($DLBITMAPS of them) is exactly part_count entries"
	else
		_skip "parts-length check: no row carries a bitmap"
	fi

	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients?include_parts=maybe"
	_assert_status 400 "include_parts must be true/false"
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients?sort=nonsuch"
	_assert_status 400 "unknown sort key on the per-file route → 400"
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients?sort=name&order=desc"
	_assert_status 200 "the /clients sort keys work on the per-file route"
	_curl -X POST -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients"
	_assert_status 405 "POST /downloads/{hash}/clients → 405"
	_curl -H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/downloads/ffffffffffffffffffffffffffffffff/clients"
	_assert_status 404 "unknown hash on the per-file route → 404"

	# The promoted client fields (issue #984) must be on the LIST row, not
	# just the detail object — the desktop renders them as table columns.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/clients?limit=1"
	if [ "$(echo "$CURL_BODY" | jq -r '.clients | length')" != "0" ]; then
		for k in source_origin available_parts mod_version view_shared_disabled; do
			_assert_json_eq ".clients[0] | has(\"$k\")" true "/clients row carries $k"
		done
	fi

	# The a4af path exists for POST only, so a GET is 405 rather than 404:
	# the resource is there, the method is not.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 405 "GET /downloads/{hash}/a4af → 405 (POST-only route)"

	# Per-source swap (issue #983): `client_ecid` narrows swap_this to one
	# source. Validation is asserted here rather than the swap itself — a
	# regtest daemon has no A4AF source to move, and the conflict/not-found
	# paths are the ones that must never silently succeed.
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_this","client_ecid":"nope"}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 400 "POST a4af with a non-integer client_ecid → 400"

	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_others","client_ecid":1}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 400 "POST a4af with client_ecid on swap_others → 400"

	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_this_auto","client_ecid":1}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 400 "POST a4af with client_ecid on swap_this_auto → 400"

	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_this","client_ecid":4294967290}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 404 "POST a4af naming an unknown client_ecid → 404"

	# A live peer that is not an A4AF source of this file is a conflict, not a
	# no-op: pick any client from /clients and ensure it is absent from the
	# A4AF list before asserting.
	OTHER_ECID=$(curl -s -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/clients?limit=1" \
		| jq -r '.clients[0].ecid // empty')
	if [ -n "$OTHER_ECID" ]; then
		IN_A4AF=$(curl -s -H "Authorization: Bearer $TOKEN" \
			"$HOST/api/v0/downloads/$HASH/clients" \
			| jq -r --argjson e "$OTHER_ECID" '[.clients[] | select(.ecid == $e and .a4af)] | length')
		if [ "$IN_A4AF" = "0" ]; then
			_curl -X POST -H "Authorization: Bearer $TOKEN" \
				-H "Content-Type: application/json" \
				-d "{\"action\":\"swap_this\",\"client_ecid\":$OTHER_ECID}" \
				"$HOST/api/v0/downloads/$HASH/a4af"
			_assert_status 409 "POST a4af for a non-A4AF client → 409"
		fi
	fi

	# Valid action → 200 (no-op on a download with no A4AF sources, but
	# exercises the EC op path). Response echoes the A4AF view.
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_others"}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 200 "POST /downloads/{hash}/a4af swap_others → 200"
	_assert_json_eq '.source_ecids | type' array \
		'POST /a4af response carries source_ecids array'

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
	# Live upload activity (issue #466).
	_assert_json_eq '.shared[0].upload_speed_bps | type' number \
		'/shared[0].upload_speed_bps is numeric (#466)'
	_assert_json_eq '.shared[0].uploading | type' number \
		'/shared[0].uploading is numeric (#466)'
	# null when never uploaded, or on a known.met entry predating the field.
	_assert_json_eq '(.shared[0].last_upload == null or (.shared[0].last_upload | type) == "number")' true \
		'/shared[0].last_upload is a number or null (#466)'
	_assert_json_eq '(.shared[0].shared_since == null or (.shared[0].shared_since | type) == "number")' true \
		'/shared[0].shared_since is a number or null (#466)'
	# Hashing progress on the shared row (issue #1054). Parts hashed so far
	# by a Verify Local Data / AICH rebuild, 0 when idle. Only the type is
	# asserted: a smoke run has no hash in flight, and racing one would make
	# the check flaky rather than stronger.
	_assert_json_eq '.shared[0].hashing_progress | type' number \
		'/shared[0].hashing_progress is numeric (#1054)'

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
	_assert_json_eq '.hashing_progress | type' number \
		'/shared/{hash} carries hashing_progress (#1054)'
	_assert_json_eq '.comment | type' string \
		'/shared/{hash} carries comment'
	_assert_json_eq '.rating | type' number \
		'/shared/{hash} carries rating'
	SHPARTCOUNT=$(printf '%s' "$CURL_BODY" | jq -r '.part_count')

	# --- 6c. GET /shared/{hash}/clients: the upload-side half of the
	# per-file client rows (issue #984). Same handler and same row shape as
	# the download side asserted above; what differs is which collection the
	# hash must belong to, which is what the 404 below pins down.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients"
	_assert_status 200 "GET /shared/{hash}/clients → 200"
	_assert_json_eq '.clients | type' array '/shared/{hash}/clients returns a clients array'
	for k in total offset limit; do
		_assert_json_eq "has(\"$k\")" true "/shared/{hash}/clients envelope has $k"
	done
	# Guarded and counted like the download side above: a shared file has
	# rows only while a peer is downloading it from us.
	SHROWS=$(printf '%s' "$CURL_BODY" | jq '.clients | length')
	echo "  --- /shared/{hash}/clients returned $SHROWS row(s) ---"
	if [ "$SHROWS" -gt 0 ]; then
		_assert_json_eq '[.clients[] | select(.role as $r | ($r == null) or ((["source","peer","both","none"] | index($r)) == null))] | length' \
			0 "every shared-side row has a valid role"
		_assert_json_eq '[.clients[] | select(.a4af == null)] | length' 0 \
			"every shared-side row has an a4af flag"
		_assert_json_eq '[.clients[] | select(has("parts"))] | length' 0 \
			"no parts bitmap on the shared route without include_parts"
	else
		_skip "shared-side row-shape checks: no peer is downloading the shared file"
	fi

	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients?include_parts=true"
	_assert_status 200 "GET /shared/{hash}/clients?include_parts=true → 200"
	SHBITMAPS=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(has("parts"))] | length')
	if [ "$SHBITMAPS" -gt 0 ]; then
		_assert_json_eq "[.clients[] | select(has(\"parts\")) | select((.parts | length) != $SHPARTCOUNT)] | length" \
			0 "every shared-side parts bitmap ($SHBITMAPS of them) is exactly part_count entries"
	else
		_skip "shared-side parts-length check: no row carries a bitmap"
	fi

	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients?include_parts=maybe"
	_assert_status 400 "include_parts must be true/false on the shared route"
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients?sort=nonsuch"
	_assert_status 400 "unknown sort key on /shared/{hash}/clients → 400"
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients?sort=name&order=desc&limit=1"
	_assert_status 200 "the /clients list params work on /shared/{hash}/clients"
	_curl -X POST -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients"
	_assert_status 405 "POST /shared/{hash}/clients → 405"

	# A hash that is downloading AND shared answers the same on both routes:
	# one handler, one row set, the direction carried per row. Speeds move
	# between two snapshots, so compare the row set and each row's relation
	# to the file rather than the whole body.
	DL_HASHES=$(curl -s --max-time 10 -H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/downloads" | jq -c '[.downloads[].hash]')
	SHARED_HASHES=$(curl -s --max-time 10 -H "Authorization: Bearer $TOKEN" \
		"$HOST/api/v0/shared" | jq -c '[.shared[].hash]')
	BOTH_HASH=$(printf '%s' "$SHARED_HASHES" | jq -r --argjson dl "$DL_HASHES" \
		'first(.[] | select(IN($dl[]))) // empty')
	SHARED_ONLY_HASH=$(printf '%s' "$SHARED_HASHES" | jq -r --argjson dl "$DL_HASHES" \
		'first(.[] | select(IN($dl[]) | not)) // empty')

	if [ -n "$BOTH_HASH" ]; then
		_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$BOTH_HASH/clients"
		_assert_status 200 "a downloading+shared hash is served by /downloads/{hash}/clients"
		DL_ROWS=$(printf '%s' "$CURL_BODY" | jq -c '[.clients[] | {ecid, role, a4af}] | sort')
		_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$BOTH_HASH/clients"
		_assert_status 200 "the same hash is served by /shared/{hash}/clients"
		_assert_json_eq '[.clients[] | {ecid, role, a4af}] | sort | tojson' "$DL_ROWS" \
			"both routes return the same rows for a downloading+shared hash"
	else
		_skip "same-rows check: no hash is both downloading and shared"
	fi

	# The 404 pair is the only difference between the two routes: each hash
	# must belong to that route's collection.
	if [ -n "$SHARED_ONLY_HASH" ]; then
		_curl -H "Authorization: Bearer $TOKEN" \
			"$HOST/api/v0/downloads/$SHARED_ONLY_HASH/clients"
		_assert_status 404 "a shared-only hash is a 404 on /downloads/{hash}/clients"
	else
		_skip "shared-only 404 check: every shared file is also downloading"
	fi
fi

# --- 6d. Missing-hash 404s on the shared routes. -------------------
# Both use a hash that exists nowhere, so neither needs a shared file and
# both belong outside the "has entries" gate above: on a daemon sharing
# nothing these are still the checks that pin the routes down.
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/shared/baadbaadbaadbaadbaadbaadbaadbaad"
_assert_status 404 "GET /shared/{nonexistent-hash} → 404"
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/shared/baadbaadbaadbaadbaadbaadbaadbaad/clients"
_assert_status 404 "unknown hash on /shared/{hash}/clients → 404"

# --- 7. Method gate. DELETE is method-gated on the /shared collection
# (no bulk-unshare endpoint); the /downloads collection now accepts a bulk
# DELETE (issue #358, exercised by 29-bulk-mutations.sh), so it is no
# longer 405 here.
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared"
_assert_status 405 "DELETE /api/v0/shared → 405"

# --- Summary. -----------------------------------------------------
echo
SKIP_NOTE=""
[ "$SKIP_COUNT" -gt 0 ] && SKIP_NOTE=" ($SKIP_COUNT check(s) skipped)"
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed$SKIP_NOTE"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
