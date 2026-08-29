#!/usr/bin/env bash
#
# amuleapi 18-categories-crud — categories CRUD.
#
# Endpoints:
#   POST   /api/v0/categories             — create
#       body: {name, save_path?, comment?, color?, priority?}
#   PATCH  /api/v0/categories/{index}     — update
#       body: any subset of {name, save_path, comment, color, priority}
#   DELETE /api/v0/categories/{index}     — remove
#
# The default (index=0) "All" category cannot be deleted —
# DELETE /categories/0 returns 400. Custom categories are 1..255.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

TEST_NAME="18-categories-crud-smoke-cat"
TEST_PATH="/tmp/18-categories-crud-cat-dir"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_18_categories_crud_body.XXXXXX)
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

_assert_body_empty() {
	local label=$1
	if [ -z "$CURL_BODY" ]; then
		_pass "$label"
	else
		_fail "$label" "expected an empty body, got: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

mkdir -p "$TEST_PATH"

echo "amuleapi 18-categories-crud smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
# --- Member GET. ---------------------------------------------------
#
# Every other resource with a member path has a member GET. This one had PATCH
# and DELETE only, so a client that had just created a category and wanted the
# stored result had to re-fetch the whole collection and search it by index.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/0"
_assert_status 200 "GET /categories/0 → 200"
_assert_json_eq '.index' 0 '/categories/0 reports index 0'

# Category 0 carries a name and a path amuled does not hold for it: its
# `defaultcat` is built with an empty title and path, which left a client
# rendering a picker with a blank row and nowhere to show where an
# uncategorised download lands. `path` is not invented -- it is
# directories.incoming_path, which is genuinely where such a file is saved.
_assert_json_eq '.name' Default '/categories/0 is named Default'
INCOMING=$(curl -s -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/preferences" | jq -r '.directories.incoming_path')
_assert_json_eq '.save_path' "$INCOMING" '/categories/0 save_path is directories.incoming_path'
# color is "#rrggbb", not the raw 24-bit integer. The core packs it as
# 0x00BBGGRR (red in the low byte), so a naive hex print of the integer
# would come out reversed -- pin the format so that cannot regress.
_assert_json_eq '(.color | test("^#[0-9a-f]{6}$"))' true \
	'/categories/0 color is a #rrggbb string'

# ...and the same values whether or not the daemon sent the row. This phase
# creates a custom category below, which is what makes amuled start emitting
# index 0 itself; asserting again after that would be the real regression
# test, so section 6b does exactly that before the cleanup.

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/250"
_assert_status 404 "GET /categories/{absent} → 404"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/999"
_assert_status 400 "GET /categories/{out-of-range} → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/0"
_assert_status 405 "PUT /categories/0 → 405"

# --- Shared list contract. -----------------------------------------
#
# /categories was the tenth list endpoint and the only one that never parsed
# ?limit/&offset/&sort/&order: the same query string was a hard error on
# /downloads and a silent no-op here, while the response still carried the
# page-meta trio a caller could not influence.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories"
_assert_status 200 "GET /categories → 200"
_assert_json_eq '.total | type'  number '/categories carries total'
_assert_json_eq '.offset | type' number '/categories carries offset'
_assert_json_eq '.limit' 100 '/categories omitted limit echoes the default 100'

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories?limit=1"
_assert_status 200 "GET /categories?limit=1 → 200"
_assert_json_eq '.categories | length' 1 '/categories?limit=1 returns one row'
_assert_json_eq '.limit' 1 '/categories?limit=1 echoes the limit'

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories?sort=index&order=desc"
_assert_status 200 "GET /categories?sort=index&order=desc → 200"

# The parameters are validated now, not ignored.
for bad in "limit=abc" "limit=1000000001" "offset=-1" "order=sideways" "sort=nonexistent_field"; do
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories?$bad"
	_assert_status 400 "GET /categories?$bad → 400"
done

_curl -X POST -H "Content-Type: application/json" \
	-d "{\"name\":\"$TEST_NAME\"}" "$HOST/api/v0/categories"
_assert_status 401 "POST /categories (no token) → 401"

_curl -X PATCH -H "Content-Type: application/json" \
	-d '{"name":"x"}' "$HOST/api/v0/categories/1"
_assert_status 401 "PATCH /categories/{idx} (no token) → 401"

_curl -X DELETE "$HOST/api/v0/categories/1"
_assert_status 401 "DELETE /categories/{idx} (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"name\":\"$TEST_NAME\"}" "$HOST/api/v0/categories"
	_assert_status 403 "POST /categories (guest) → 403"
fi

# --- 2. POST /categories (create). --------------------------------
#
# Snapshot the indexes first. The create answers with no body, so the new
# index is whichever one the collection did not hold before -- which beats
# looking the name up afterwards, since nothing stops the daemon from already
# holding a category by that name (a previous run of this smoke that died
# before its cleanup, say), and a name lookup would then hand back the older
# row and delete that instead.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories?limit=500"
BEFORE_IDX=$(printf '%s' "$CURL_BODY" | jq -c '[.categories[].index]')

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"name\":\"$TEST_NAME\",\"save_path\":\"$TEST_PATH\",\"comment\":\"18-categories-crud test\",\"priority\":\"high\"}" \
	"$HOST/api/v0/categories"
# 202 with no body. EC_OP_CREATE_CATEGORY answers success or failure and never
# returns the index it assigned, so naming the new category here meant scanning
# the snapshot for one with a matching name and falling back to a bodiless 201
# when the scan came up short. The caller re-reads the collection, which is what
# the lookup below does.
_assert_status 202 "POST /categories (create) → 202"
_assert_body_empty 'create sends no body'

# Verify by GET /categories, which is also where the assigned index comes from.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories?limit=500"
NEW_IDX=$(printf '%s' "$CURL_BODY" \
	| jq -r --argjson b "$BEFORE_IDX" \
	  '[.categories[].index] - $b | first // empty')
if [ -n "$NEW_IDX" ]; then
	_pass "the created category is readable from /categories (index=$NEW_IDX)"
else
	_die "created category not in /categories: ${CURL_BODY:0:300}"
fi
# The stored row is what the POST body asked for. Asserted here rather than off
# a create response, which is the whole reason the create carries no body: this
# reads what the daemon actually kept, not what amuleapi guessed it kept.
LOOKUP=$(printf '%s' "$CURL_BODY" \
	| jq --argjson i "$NEW_IDX" '[.categories[] | select(.index == $i)] | first')
for f in name:"$TEST_NAME" save_path:"$TEST_PATH" priority:high; do
	key=${f%%:*}; want=${f#*:}
	got=$(printf '%s' "$LOOKUP" | jq -r ".$key")
	if [ "$got" = "$want" ]; then
		_pass "created category round-trips $key=$want"
	else
		_fail "created category $key" "expected $want, got $got"
	fi
done

# --- 3. POST error paths. -----------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/categories"
_assert_status 400 "POST /categories (no name) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"name\":\"x\",\"priority\":\"bogus\"}" "$HOST/api/v0/categories"
_assert_status 400 "POST /categories (bad priority enum) → 400"

# R9: a category's priority is rendered on read from the full six-level file
# set, so the write side accepts the same six. very_low and release used to be
# refused here, which meant a read-modify-write round-trip failed on a field
# the client never touched.
for p in very_low release; do
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"name\":\"prio-$p\",\"save_path\":\"$TEST_PATH\",\"priority\":\"$p\"}" \
		"$HOST/api/v0/categories"
	_assert_status 202 "POST /categories (priority=$p accepted, R9) → 202"
	# Leave no residue: a leftover category poisons the next run's create.
	PRIO_IDX=$(curl -s -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories" \
		| jq -r --arg n "prio-$p" '[.categories[] | select(.name == $n)][0].index // empty')
	[ -n "$PRIO_IDX" ] && curl -s -o /dev/null -X DELETE \
		-H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/$PRIO_IDX"
done

# --- 4. PATCH /categories/{idx}. ----------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"comment":"updated by 18-categories-crud","priority":"low"}' \
	"$HOST/api/v0/categories/$NEW_IDX"
_assert_status 200 "PATCH /categories/$NEW_IDX → 200"
_assert_json_eq '.comment'  'updated by 18-categories-crud' 'PATCH response shows new comment'
_assert_json_eq '.priority' low                  'PATCH response shows priority=low'

# Immediate GET (no-stale).
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories"
OBS_COMMENT=$(printf '%s' "$CURL_BODY" \
	| jq -r --arg n "$TEST_NAME" \
	  '.categories[] | select(.name == $n) | .comment')
if [ "$OBS_COMMENT" = "updated by 18-categories-crud" ]; then
	_pass "IMMEDIATE GET /categories shows updated comment (no stale)"
else
	_fail "GET /categories staleness after PATCH" \
		"expected 'updated by 18-categories-crud', got '$OBS_COMMENT'"
fi

# --- 5. PATCH error paths. ----------------------------------------
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"bogus"}' "$HOST/api/v0/categories/$NEW_IDX"
_assert_status 400 "PATCH /categories bogus enum → 400"

for p in very_low release; do
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"priority\":\"$p\"}" "$HOST/api/v0/categories/$NEW_IDX"
	_assert_status 200 "PATCH /categories (priority=$p accepted, R9) → 200"
done
# ...and back to a value the rest of the phase expects.
_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"priority":"high"}' "$HOST/api/v0/categories/$NEW_IDX"
_assert_status 200 "PATCH /categories priority restored to high → 200"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"name":"unknown"}' "$HOST/api/v0/categories/199"
_assert_status 404 "PATCH /categories unknown index → 404"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"name":"x"}' "$HOST/api/v0/categories/not-a-number"
_assert_status 400 "PATCH /categories non-numeric index → 400"

# --- 5b. Category 0 reads the same now that amuled sends the row. ---
#
# amuled's EC omits index 0 entirely until a custom category exists, and the
# create above added one -- so this is the same read as section 1, on the other
# side of that switch. Filling name/path in only for the synthesised row would
# have made /categories/0 answer "Default" before this point and "" after it,
# which is a response shape that depends on unrelated state.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/0"
_assert_status 200 "GET /categories/0 (amuled now sends it) → 200"
_assert_json_eq '.name' Default '/categories/0 is still named Default'
_assert_json_eq '.save_path' "$INCOMING" '/categories/0 save_path is still directories.incoming_path'

# The collection agrees with the member route, on the same daemon state.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories?limit=500"
_assert_json_eq '[.categories[] | select(.index == 0)][0].name' Default \
	'/categories lists index 0 as Default too'
_assert_json_eq '[.categories[] | select(.index == 0)][0].save_path' "$INCOMING" \
	'/categories lists index 0 with the incoming save_path too'

# --- 6. DELETE happy path + cannot-delete-default + no-stale. ----
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/categories/0"
_assert_status 400 "DELETE /categories/0 (default) → 400"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/categories/$NEW_IDX"
# 204, no body: the index came from the URL and `ok` restated the status code.
_assert_status 204 "DELETE /categories/$NEW_IDX → 204"
_assert_body_empty 'DELETE sends no body'

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories"
STILL=$(printf '%s' "$CURL_BODY" \
	| jq --arg n "$TEST_NAME" \
	  '[.categories[] | select(.name == $n)] | length')
if [ "$STILL" = "0" ]; then
	_pass "Deleted category gone from /categories list (no stale)"
else
	_fail "/categories staleness after DELETE" \
		"$TEST_NAME still present after DELETE"
fi

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/categories/$NEW_IDX"
_assert_status 404 "DELETE same index twice → 404"

# --- 8. The path-substitution contract (partial success, not failure). ---
#
# amuled answers EC_OP_FAILED for these two ops when it created or updated the
# category but could not use the path, keeping another one and returning it in
# EC_TAG_CATEGORY_PATH. The category exists either way, so the API reports
# success and `save_path` carries the path that was actually kept. Relaying it
# as a 400 used to claim the request failed while the category was sitting in
# the list.
#
# Placed last because each case creates a category; each cleans up its own.

# 8a. No save_path at all. The daemon's documented default is the incoming
#     directory, so this is an ordinary create, not an error.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"name":"18-cat-nopath"}' "$HOST/api/v0/categories"
_assert_status 202 "POST /categories with no save_path -> 202 (defaults to incoming)"
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories"
NOPATH_IDX=$(printf '%s' "$CURL_BODY" \
	| jq -r '[.categories[] | select(.name == "18-cat-nopath")][0].index // empty')
if [ -n "$NOPATH_IDX" ]; then
	_pass "the category created without save_path is in the list (index=$NOPATH_IDX)"
	_assert_json_eq '[.categories[] | select(.name == "18-cat-nopath")][0].save_path' \
		"$INCOMING" 'a create with no save_path lands on directories.incoming_path'
	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/$NOPATH_IDX"
	_assert_status 204 "cleanup: DELETE /categories/$NOPATH_IDX -> 204"
else
	_fail "create without save_path" "202 returned but no such category in /categories"
fi

# 8b. A save_path the daemon cannot create. It keeps the incoming directory and
#     the category still exists. Running as root the mkdir would succeed, so the
#     assertion is guarded on the substitution actually having happened.
UNUSABLE=/18-cat-unusable-$$/sub
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"name\":\"18-cat-badpath\",\"save_path\":\"$UNUSABLE\"}" "$HOST/api/v0/categories"
_assert_status 202 "POST /categories with an uncreatable save_path -> 202 (path substituted)"
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories"
BAD_IDX=$(printf '%s' "$CURL_BODY" \
	| jq -r '[.categories[] | select(.name == "18-cat-badpath")][0].index // empty')
if [ -n "$BAD_IDX" ]; then
	_pass "the category survives an uncreatable save_path (index=$BAD_IDX)"
	BAD_PATH=$(printf '%s' "$CURL_BODY" \
		| jq -r '[.categories[] | select(.name == "18-cat-badpath")][0].save_path')
	if [ "$BAD_PATH" = "$UNUSABLE" ]; then
		_skip "path-substitution check: the daemon could create $UNUSABLE (running as root?)"
	else
		_assert_json_eq '[.categories[] | select(.name == "18-cat-badpath")][0].save_path' \
			"$INCOMING" 'the refused path is replaced by directories.incoming_path, not stored'
	fi

	# 8c. The same contract on PATCH: an update whose path is refused still
	#     applies every other field, and the echoed object reports the path
	#     that was kept rather than the one that was asked for.
	_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"name\":\"18-cat-badpath-renamed\",\"save_path\":\"$UNUSABLE\"}" \
		"$HOST/api/v0/categories/$BAD_IDX"
	_assert_status 200 "PATCH /categories/$BAD_IDX with an uncreatable save_path -> 200"
	_assert_json_eq '.name' '18-cat-badpath-renamed' \
		'the rest of the PATCH lands even when the path is refused'
	_assert_json_eq "(.save_path == \"$UNUSABLE\")" false \
		'the echoed save_path is the kept one, not the refused one'

	_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories/$BAD_IDX"
	_assert_status 204 "cleanup: DELETE /categories/$BAD_IDX -> 204"
else
	_fail "create with an uncreatable save_path" "202 returned but no such category in /categories"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
