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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 04-read-downloads-shared smoke @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
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
	_assert_json_eq '.downloads[0].size_bytes | type' number \
		'/downloads[0].size_bytes is numeric'
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
	_assert_json_eq '.downloads[0].kad_comment_lookup_running | type' boolean \
		'/downloads[0].kad_comment_lookup_running is boolean (issue #434)'
	# Moved onto the list by issue #1054 — it used to be detail-only, which
	# left a list-driven client with no way to see a hash running.
	_assert_json_eq '.downloads[0].hashed_part_count | type' number \
		'/downloads[0].hashed_part_count is numeric (#1054)'

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
	_assert_json_eq '.parts_total_count | type' number \
		'/downloads/{hash} carries parts_total_count'
	# null when stalled/paused: nothing to compute an ETA from. It was -1,
	# which a client had to know meant "unknown".
	_assert_json_eq '(.remaining_seconds == null or (.remaining_seconds | type) == "number")' true \
		'/downloads/{hash} remaining_seconds is a number or null'
	# Same rule: null, not a 0 that reads as 1970, when no complete copy of
	# the file has ever been seen across the current sources.
	_assert_json_eq '(.last_seen_complete_at == null or (.last_seen_complete_at | type) == "number")' true \
		'/downloads/{hash} last_seen_complete_at is a number or null'
	_assert_json_eq '.last_seen_complete_at != 0' true \
		'/downloads/{hash} last_seen_complete_at never uses 0 as "never"'
	# R10: null until the hashset exists, never the "" sentinel it used to be.
	_assert_json_eq '(.aich_hash == null or (.aich_hash | type) == "string")' true \
		'/downloads/{hash} aich_hash is a string or null, never ""'
	_assert_json_eq '.aich_hash != ""' true \
		'/downloads/{hash} aich_hash never uses the empty-string sentinel'
	_assert_json_eq '.part_file_name | type' string \
		'/downloads/{hash} carries part_file_name'
	_assert_json_eq '.directory | type' string \
		'/downloads/{hash} carries directory (#417)'
	_assert_json_eq '.upload_queue_count | type' number \
		'/downloads/{hash} carries upload_queue_count'
	_assert_json_eq '.my_comment | type' string \
		'/downloads/{hash} carries my_comment (yours, not comments[].comment)'
	_assert_json_eq '.my_rating | type' number \
		'/downloads/{hash} carries my_rating (yours, not comments[].rating)'
	_assert_json_eq '.a4af_auto | type' boolean \
		'/downloads/{hash} carries a4af_auto'

	# Per-source comments sub-resource (issue #419).
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/comments"
	_assert_status 200 "GET /downloads/{hash}/comments → 200"
	_assert_json_eq '.total | type' number \
		'/downloads/{hash}/comments carries numeric total'
	_assert_json_eq '.comments | type' array \
		'/downloads/{hash}/comments.comments is an array'
	_assert_json_eq '.kad_comment_lookup_running | type' boolean \
		'/downloads/{hash}/comments carries kad_comment_lookup_running flag'

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

	# `swap_this_auto` was a third action here and is refused, not ignored:
	# it flipped a flag rather than moving sources, and a flip cannot be
	# retried safely. The message names where the flag is set instead.
	_curl -X POST -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"action":"swap_this_auto"}' "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 400 "POST a4af swap_this_auto → 400 (moved to PATCH)"
	_assert_json_eq '.error.message | test("a4af_auto")' true \
		'the swap_this_auto 400 names the PATCH field'

	# --- a4af_auto is a set, and setting it twice is not an undo. -------
	#
	# This is the whole point of moving it: EC_OP_PARTFILE_SWAP_A4AF_THIS_AUTO
	# maps to SetA4AFAuto(!IsA4AFAuto()), so the old action landed on the
	# opposite value whenever a request was repeated -- which an HTTP library
	# or a browser can do without the caller knowing.
	for want in true false true; do
		_curl -X PATCH -H "Authorization: Bearer $TOKEN" \
			-H "Content-Type: application/json" \
			-d "{\"a4af_auto\":$want}" "$HOST/api/v0/downloads/$HASH"
		_assert_status 200 "PATCH a4af_auto=$want → 200"
		_assert_json_eq '.a4af_auto' "$want" "PATCH a4af_auto=$want reads back $want"

		# Same body again: the value must not move.
		_curl -X PATCH -H "Authorization: Bearer $TOKEN" \
			-H "Content-Type: application/json" \
			-d "{\"a4af_auto\":$want}" "$HOST/api/v0/downloads/$HASH"
		_assert_status 200 "PATCH a4af_auto=$want again → 200"
		_assert_json_eq '.a4af_auto' "$want" \
			"a repeated PATCH a4af_auto=$want is a no-op, not a flip"

		# ...and a re-read agrees, so the PATCH body is not the only place
		# the new value exists.
		_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH"
		_assert_json_eq '.a4af_auto' "$want" "GET after PATCH reports $want"
	done

	# A non-boolean is a 400 rather than a coerced truthy value.
	_curl -X PATCH -H "Authorization: Bearer $TOKEN" \
		-H "Content-Type: application/json" \
		-d '{"a4af_auto":"yes"}' "$HOST/api/v0/downloads/$HASH"
	_assert_status 400 "PATCH a4af_auto non-boolean → 400"

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
		_assert_json_eq '[.clients[] | select(.role as $r | ($r == null) or ((["downloading_from","uploading_to","both","none"] | index($r)) == null))] | length' \
			0 "every row has a valid role"
		_assert_json_eq '[.clients[] | select(.a4af == null)] | length' 0 "every row has an a4af flag"
		_assert_json_eq '[.clients[] | select(has("parts"))] | length' 0 "no parts bitmap without include_parts"
		# The two part indices ride include_parts with the bitmap: an
		# index into a bitmap the caller did not ask for is unreadable,
		# so neither key may appear here either.
		_assert_json_eq '[.clients[] | select(has("next_requested_part_index") or has("downloading_part_index"))] | length' \
			0 "no part indices without include_parts"
	else
		_skip "row-shape checks: no peer is connected to the download"
	fi

	# Opt-in bitmaps are exactly parts_total_count long for a row that has one.
	PARTCOUNT=$(curl -s -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH" | jq -r '.progress.parts | length')
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients?include_parts=true"
	_assert_status 200 "GET /downloads/{hash}/clients?include_parts=true → 200"
	DLBITMAPS=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(has("parts"))] | length')
	if [ "$DLBITMAPS" -gt 0 ]; then
		_assert_json_eq "[.clients[] | select(has(\"parts\")) | select((.parts | length) != $PARTCOUNT)] | length" \
			0 "every parts bitmap ($DLBITMAPS of them) is exactly parts_total_count entries"
	else
		_skip "parts-length check: no row carries a bitmap"
	fi

	# R7: a sort value is spelled exactly like the response key it orders by,
	# so a field rename can never orphan one. These three moved with the keys;
	# the old spellings must now be rejected rather than silently accepted.
	for _sk in size_bytes progress.percent speed_bytes_per_second hash name status; do
		_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?sort=$_sk&limit=1"
		_assert_status 200 "/downloads?sort=$_sk (R7: sort value == response key) → 200"
	done
	for _sk in size progress speed; do
		_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads?sort=$_sk&limit=1"
		_assert_status 400 "/downloads?sort=$_sk (pre-rename spelling) → 400"
	done
	# The two part indices the desktop's source bar paints over the bitmap.
	# Under include_parts both keys are ALWAYS present on every row, null
	# rather than omitted when the index does not apply, so one query yields
	# one row shape -- assert presence separately from type, or a missing key
	# and a null one both read as `null` and the test proves nothing.
	#
	# Re-issued rather than reusing the include_parts body fetched above: the
	# sort loops in between have each overwritten $CURL_BODY, and the last of
	# them left a 400 error envelope there. `.clients` on that is null, `null
	# | length` is 0, and the whole block below would have skipped itself on
	# every run while reporting "no peer is connected".
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/clients?include_parts=true"
	_assert_status 200 "GET /downloads/{hash}/clients?include_parts=true (part-index block) → 200"
	DLIDXROWS=$(printf '%s' "$CURL_BODY" | jq '.clients | length')
	echo "  --- part-index block sees $DLIDXROWS row(s) ---"
	if [ "$DLIDXROWS" -gt 0 ]; then
		for k in next_requested_part_index downloading_part_index; do
			_assert_json_eq "[.clients[] | select(has(\"$k\") | not)] | length" 0 \
				"every row carries $k under include_parts"
			# Number or null, never a string and never a bool: null is
			# how "the peer never reported it", the 0xffff "nothing
			# pending" sentinel, and a non-source row are all spelled.
			_assert_json_eq "[.clients[] | select((.$k | type) as \$t | \$t != \"number\" and \$t != \"null\")] | length" \
				0 "$k is a number or null on every row"
			# 0 is a real chunk index, so an unknown value must be a
			# JSON null and not a 0 standing in for one; and a known
			# value must address a chunk of THIS file.
			_assert_json_eq "[.clients[] | select(.$k != null) | select(.$k < 0 or .$k >= $PARTCOUNT)] | length" \
				0 "$k, when not null, is an index inside [0, part_count)"
		done
		# A row that is not a source for this file has no download bitmap,
		# so its indices belong to some other file: both must be null.
		_assert_json_eq '[.clients[] | select(.role == "uploading_to" or .role == "none") | select(.next_requested_part_index != null or .downloading_part_index != null)] | length' \
			0 "non-source rows report both part indices as null"
		# An index is only meaningful against the bitmap it indexes, and
		# role alone does not guarantee one: a source that has sent no
		# part status yet, or whose decoded bitmap cannot cover the file,
		# ships no `parts` key at all. Such a row is a stripe coordinate
		# with no bar to paint it on, so both indices must be null there
		# too. Count the rows that actually exercised it, so a green run
		# with no such row cannot be read as having proved the gate.
		DLNOBITS=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(has("parts") | not)] | length')
		_assert_json_eq '[.clients[] | select(has("parts") | not) | select(.next_requested_part_index != null or .downloading_part_index != null)] | length' \
			0 "a row without a parts bitmap reports both part indices as null ($DLNOBITS such row(s))"
		# The state guard: the daemon reports downloading_part_index as a
		# stale 0 for a source that is merely connected or queued, which
		# would have a renderer paint "downloading now" on chunk 0 of
		# every idle row. Anything but download_state "downloading" must
		# come back null -- and null, not 0, is what distinguishes it
		# from a peer genuinely feeding chunk 0.
		DLIDLE=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(.role == "downloading_from" or .role == "both") | select(.download_state != "downloading")] | length')
		if [ "$DLIDLE" -gt 0 ]; then
			_assert_json_eq '[.clients[] | select(.role == "downloading_from" or .role == "both") | select(.download_state != "downloading") | select(.downloading_part_index != null)] | length' \
				0 "a source that is not downloading reports downloading_part_index null, not 0 ($DLIDLE such row(s))"
		else
			_skip "idle-source downloading_part_index check: every source row is actively downloading"
		fi
		# Report which path the run actually exercised, so a green run
		# cannot be read as having seen a live peer feeding a chunk.
		echo "  --- non-null part indices observed: next=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(.next_requested_part_index != null)] | length') last=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(.downloading_part_index != null)] | length') of $DLIDXROWS row(s) ---"
	else
		_skip "part-index checks: no peer is connected to the download"
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
		for k in source_origin parts_offered_count client_mod_name shared_files_browsable; do
			_assert_json_eq ".clients[0] | has(\"$k\")" true "/clients row carries $k"
		done
	fi

	# The a4af path exists for POST only, so a GET is 405 rather than 404:
	# the resource is there, the method is not.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/downloads/$HASH/a4af"
	_assert_status 405 "GET /downloads/{hash}/a4af → 405 (POST-only route)"

	# Per-source swap (issue #983): `client_ecid` narrows swap_this to one
	# source. Validation is asserted here rather than the swap itself — a
	# regtest daemon has no A4AF source to move, and the rejection/not-found
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

	# A live client that is not an A4AF source of this file is a rejection, not a
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
			_assert_json_eq '.error.code' not_a4af_source \
				'the 409 names not_a4af_source, not a bare conflict'
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
	# xfer / requests / accepts were flattened (R11).
	_assert_json_eq '.shared[0] | has("xfer")' false \
		'/shared[0] no longer wraps counters in xfer'
	_assert_json_eq '.shared[0].uploaded_bytes_total | type' number \
		'/shared[0].uploaded_bytes_total is numeric'
	_assert_json_eq '.shared[0].priority | type' string \
		'/shared[0].priority is string'
	_assert_json_eq '.shared[0].priority_auto | type' boolean \
		'/shared[0].priority_auto is boolean'
	# Live upload activity (issue #466).
	_assert_json_eq '.shared[0].upload_speed_bytes_per_second | type' number \
		'/shared[0].upload_speed_bytes_per_second is numeric (#466)'
	_assert_json_eq '.shared[0].uploading_client_count | type' number \
		'/shared[0].uploading_client_count is numeric (#466)'
	# null when never uploaded, or on a known.met entry predating the field.
	_assert_json_eq '(.shared[0].last_upload_at == null or (.shared[0].last_upload_at | type) == "number")' true \
		'/shared[0].last_upload_at is a number or null (#466)'
	_assert_json_eq '(.shared[0].shared_since_at == null or (.shared[0].shared_since_at | type) == "number")' true \
		'/shared[0].shared_since_at is a number or null (#466)'
	# Hashing progress on the shared row (issue #1054). Parts hashed so far
	# by a Verify Local Data / AICH rebuild, 0 when idle. Only the type is
	# asserted: a smoke run has no hash in flight, and racing one would make
	# the check flaky rather than stronger.
	_assert_json_eq '.shared[0].hashed_part_count | type' number \
		'/shared[0].hashed_part_count is numeric (#1054)'

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
	_assert_json_eq '.upload_ratio | type' number \
		'/shared/{hash} carries upload_ratio'
	_assert_json_eq '.directory | type' string \
		'/shared/{hash} carries directory'
	_assert_json_eq '.sources | type' object \
		'/shared/{hash} carries sources'
	_assert_json_eq '.aich_hash | type' string \
		'/shared/{hash} carries aich_hash'
	_assert_json_eq '.parts_total_count | type' number \
		'/shared/{hash} carries parts_total_count'
	_assert_json_eq '.hashed_part_count | type' number \
		'/shared/{hash} carries hashed_part_count (#1054)'
	_assert_json_eq '.my_comment | type' string \
		'/shared/{hash} carries my_comment'
	_assert_json_eq '.my_rating | type' number \
		'/shared/{hash} carries my_rating'
	SHPARTCOUNT=$(printf '%s' "$CURL_BODY" | jq -r '.parts_total_count')

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
		_assert_json_eq '[.clients[] | select(.role as $r | ($r == null) or ((["downloading_from","uploading_to","both","none"] | index($r)) == null))] | length' \
			0 "every shared-side row has a valid role"
		_assert_json_eq '[.clients[] | select(.a4af == null)] | length' 0 \
			"every shared-side row has an a4af flag"
		_assert_json_eq '[.clients[] | select(has("parts"))] | length' 0 \
			"no parts bitmap on the shared route without include_parts"
		_assert_json_eq '[.clients[] | select(has("next_requested_part_index") or has("downloading_part_index"))] | length' \
			0 "no part indices on the shared route without include_parts"
	else
		_skip "shared-side row-shape checks: no peer is downloading the shared file"
	fi

	# Same two keys on the shared route, which is the same handler. A shared
	# file's rows are overwhelmingly role "uploading_to" -- someone pulling from us --
	# and a peer's download indices describe whatever IT is downloading, not
	# this file, so the interesting case here is that they come back null.
	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients?include_parts=true"
	_assert_status 200 "GET /shared/{hash}/clients?include_parts=true → 200"
	SHIDXROWS=$(printf '%s' "$CURL_BODY" | jq '.clients | length')
	if [ "$SHIDXROWS" -gt 0 ]; then
		for k in next_requested_part_index downloading_part_index; do
			_assert_json_eq "[.clients[] | select(has(\"$k\") | not)] | length" 0 \
				"every shared-side row carries $k under include_parts"
			_assert_json_eq "[.clients[] | select((.$k | type) as \$t | \$t != \"number\" and \$t != \"null\")] | length" \
				0 "$k is a number or null on every shared-side row"
			_assert_json_eq "[.clients[] | select(.$k != null) | select(.$k < 0 or .$k >= $SHPARTCOUNT)] | length" \
				0 "$k, when not null, indexes a chunk of the shared file"
		done
		_assert_json_eq '[.clients[] | select(.role == "uploading_to" or .role == "none") | select(.next_requested_part_index != null or .downloading_part_index != null)] | length' \
			0 "shared-side non-source rows report both part indices as null"
		# An index is only meaningful against the bitmap it indexes, and
		# role alone does not guarantee one: a source that has sent no
		# part status yet, or whose decoded bitmap cannot cover the file,
		# ships no `parts` key at all. Such a row is a stripe coordinate
		# with no bar to paint it on, so both indices must be null there
		# too. Count the rows that actually exercised it, so a green run
		# with no such row cannot be read as having proved the gate.
		SHNOBITS=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(has("parts") | not)] | length')
		_assert_json_eq '[.clients[] | select(has("parts") | not) | select(.next_requested_part_index != null or .downloading_part_index != null)] | length' \
			0 "a shared-side row without a parts bitmap reports both part indices as null ($SHNOBITS such row(s))"
		SHIDLE=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(.role == "downloading_from" or .role == "both") | select(.download_state != "downloading")] | length')
		if [ "$SHIDLE" -gt 0 ]; then
			_assert_json_eq '[.clients[] | select(.role == "downloading_from" or .role == "both") | select(.download_state != "downloading") | select(.downloading_part_index != null)] | length' \
				0 "a shared-side source that is not downloading reports downloading_part_index null ($SHIDLE such row(s))"
		else
			_skip "shared-side idle-source downloading_part_index check: no idle source row"
		fi
	else
		_skip "shared-side part-index checks: no peer is downloading the shared file"
	fi

	_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/shared/$SHASH/clients?include_parts=true"
	_assert_status 200 "GET /shared/{hash}/clients?include_parts=true → 200"
	SHBITMAPS=$(printf '%s' "$CURL_BODY" | jq '[.clients[] | select(has("parts"))] | length')
	if [ "$SHBITMAPS" -gt 0 ]; then
		_assert_json_eq "[.clients[] | select(has(\"parts\")) | select((.parts | length) != $SHPARTCOUNT)] | length" \
			0 "every shared-side parts bitmap ($SHBITMAPS of them) is exactly parts_total_count entries"
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
