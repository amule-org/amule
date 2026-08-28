#!/usr/bin/env bash
#
# amuleapi list pagination + sorting (issue #357, keyset paging #1173).
# Exercises the shared ?limit/&offset/&sort/&order/&after machinery across the
# list endpoints: /downloads, /shared, /servers, /clients, /categories,
# /friends, /known_clients, /chats and /search/{id}/results.
#
# Data-tolerant like the other read smokes — it asserts the pagination
# metadata (`total`/`offset`/`limit`), that `limit` bounds the array length,
# that an omitted `limit` selects the default page of 100, and that malformed
# params are rejected with 400. Per-endpoint `sort` is only asserted accepted
# (valid field) or rejected (unknown field); the two keyset sweeps below do
# check coverage against real data (a string `hash` anchor on /shared, a
# numeric `index` anchor on /categories).
#
# Bring-up convention (see run-all.sh / 04-read-downloads-shared.sh):
#   amuleapi --config-dir=/tmp/... --host=127.0.0.1 --port=4712 \
#            --password=amule --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/... --host=127.0.0.1 --port=4712 \
#            --password=amule &
#   ./28-list-pagination-sort.sh

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_28_list_pagination_body.XXXXXX)
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

# jq expr must evaluate to a number; assert it is <= bound.
_assert_json_le() {
	local expr=$1 bound=$2 label=$3
	local actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null)
	if [ -n "$actual" ] && [ "$actual" != "null" ] && [ "$actual" -le "$bound" ] 2>/dev/null; then
		_pass "$label ($actual <= $bound)"
	else
		_fail "$label" "expected <= $bound, got $actual" "body: $CURL_BODY"
	fi
}

# jq expr must evaluate to a number; assert it is >= bound. A non-numeric
# result (a failed GET yields the string "null") fails rather than skips.
_assert_json_ge() {
	local expr=$1 bound=$2 label=$3
	local actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null)
	if [ -n "$actual" ] && [ "$actual" != "null" ] && [ "$actual" -ge "$bound" ] 2>/dev/null; then
		_pass "$label ($actual >= $bound)"
	else
		_fail "$label" "expected >= $bound, got $actual" "body: $CURL_BODY"
	fi
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 28-list-pagination-sort smoke @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for pagination tests"

sleep 3 # let the refresher build its caches

AUTH=(-H "Authorization: Bearer $TOKEN")

# The search list is addressed per search, so start one to have an id
# rather than relying on a removed implicit default.
SEARCH_SID=$(curl -s -X POST "${AUTH[@]}" -H "Content-Type: application/json" \
	-d '{"query":"amuleapi-phase28","type":"local"}' "$HOST/api/v0/search" \
	| jq -r '.id // empty')
[ -n "$SEARCH_SID" ] || _die "POST /search returned no search_id"

# endpoint:array-key pairs. The search results list wraps under "results".
ENDPOINTS=(
	"downloads:downloads"
	"shared:shared"
	"servers:servers"
	"clients:clients"
	"categories:categories"
	"friends:friends"
	"known_clients:known_clients"
	"chats:chats"
	"search/$SEARCH_SID/results:results"
)

for pair in "${ENDPOINTS[@]}"; do
	ep=${pair%%:*}
	key=${pair##*:}
	echo "  --- /$ep (key .$key) ---"

	# 1. Baseline: array + always-present pagination metadata.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep"
	_assert_status 200 "GET /$ep → 200"
	_assert_json_eq ".$key | type"  array  "/$ep .$key is an array"
	_assert_json_eq ".total | type"  number "/$ep total is a number"
	_assert_json_eq ".offset | type" number "/$ep offset is a number"
	# `limit` always reports a real page size now: an omitted parameter selects
	# the default page rather than the whole collection, so {total, offset,
	# limit} round-trips straight back into the next request. It is not the row
	# count -- that is `total`, asserted right above.
	_assert_json_eq ".limit"         100    "/$ep omitted limit echoes the default 100"
	_assert_json_le ".$key | length" 100    "/$ep omitted limit returns at most 100 rows"
	_assert_json_eq ". | has(\"limit\")" true "/$ep still carries the limit key"
	_assert_json_eq ".offset"        0      "/$ep default offset is 0"

	# 2. limit bounds the array length; limit echoes back.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=1"
	_assert_status 200 "GET /$ep?limit=1 → 200"
	_assert_json_le ".$key | length" 1 "/$ep?limit=1 returns <= 1 item"
	_assert_json_eq ".limit" 1 "/$ep?limit=1 echoes limit=1"

	# ...and it comes back as a number, confirming the default 100 asserted
	# above is a real page size (#1179 dropped the old null "no window" echo).
	_assert_json_eq ".limit | type" number "/$ep?limit=1 limit is a number"

	# 3. limit=0 → empty window, total still reported.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=0"
	_assert_status 200 "GET /$ep?limit=0 → 200"
	_assert_json_eq ".$key | length" 0 "/$ep?limit=0 returns empty array"

	# 4. offset past the end → empty window, no error.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?offset=100000"
	_assert_status 200 "GET /$ep?offset=100000 → 200"
	_assert_json_eq ".$key | length" 0 "/$ep offset past end → empty array"

	# 5. valid sort field is accepted (every list has a `name` field).
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?sort=name&order=desc"
	_assert_status 200 "GET /$ep?sort=name&order=desc → 200"

	# 6. Malformed params → 400.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=-1"
	_assert_status 400 "GET /$ep?limit=-1 → 400"
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=notanumber"
	_assert_status 400 "GET /$ep?limit=notanumber → 400"
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?offset=-5"
	_assert_status 400 "GET /$ep?offset=-5 → 400"
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?order=sideways"
	_assert_status 400 "GET /$ep?order=sideways → 400"
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?sort=nonexistent_field"
	_assert_status 400 "GET /$ep?sort=nonexistent_field → 400"

	# 7. The ceiling is 1e9, past any collection that can exist, so a caller
	# asking for the whole set gets it and a fat-fingered value is still a
	# rejection rather than a silent reduction.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=1000000001"
	_assert_status 400 "GET /$ep?limit=1000000001 → 400 (over the ceiling)"
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=99999"
	_assert_status 200 "GET /$ep?limit=99999 → 200 (legal now; was the old cap's rejection)"
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=1000000000"
	_assert_status 200 "GET /$ep?limit=1000000000 → 200 (the ceiling is in range)"
	_assert_json_eq ".limit" 1000000000 "/$ep?limit=1000000000 echoes the limit it used"

	# 8. The overflow case a reviewer cannot see by reading the diff. The
	# window used to compute `min(offset + limit, total)`, so the addition
	# happened before the clamp and wrapped on a 32-bit size_t; an inverted
	# iterator range is undefined behaviour, not a large page. The count is
	# clamped before it is added now.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=1000000000&offset=5"
	_assert_status 200 "GET /$ep?limit=1e9&offset=5 → 200 (no overflow)"
	_assert_json_eq ".offset" 5 "/$ep?offset=5 echoes offset=5"

	# 9. `after` needs an identity sort and ascending order; both refusals are
	# explicit rather than a silent fall back to the first page, which would
	# read to a paging client as "the collection never grows".
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?after=zzz"
	_assert_status 400 "GET /$ep?after= without sort → 400"
done

# --- Keyset sweep: walk a whole collection with no row repeated or skipped.
#
# The property that offset paging cannot provide. `/shared` is the collection a
# real client actually has to page, so sweep that one for real: anchor on the
# identity column, take one row at a time so the walk is many pages rather than
# one, and check the union against an unpaged fetch.
#
# Deliberately NOT asserted with offset: a row removed from an already-fetched
# page slides every later index down by one, so an offset walk skips a row --
# and since that row was not itself added, updated or removed, no SSE event
# repairs it. That is the whole reason `after` exists.
_curl "${AUTH[@]}" "$HOST/api/v0/shared?limit=1000000000&sort=hash"
SWEEP_TOTAL=$(printf '%s' "$CURL_BODY" | jq -r '.total')
ALL_HASHES=$(printf '%s' "$CURL_BODY" | jq -r '.shared[].hash' | sort)
echo "    info: /shared holds $SWEEP_TOTAL rows"

if [ "$SWEEP_TOTAL" -gt 0 ]; then
	SWEPT=""
	AFTER=""
	PAGES=0
	# Bounded so a bug cannot spin: one row per page means at most TOTAL
	# pages, plus one for the short final page that ends the walk.
	while [ "$PAGES" -le "$SWEEP_TOTAL" ]; do
		if [ -z "$AFTER" ]; then
			_curl "${AUTH[@]}" "$HOST/api/v0/shared?sort=hash&limit=1"
		else
			_curl "${AUTH[@]}" "$HOST/api/v0/shared?sort=hash&limit=1&after=$AFTER"
		fi
		[ "$CURL_STATUS" = "200" ] || break
		GOT=$(printf '%s' "$CURL_BODY" | jq -r '.shared | length')
		[ "$GOT" -eq 0 ] && break
		AFTER=$(printf '%s' "$CURL_BODY" | jq -r '.shared[-1].hash')
		SWEPT="$SWEPT$AFTER
"
		PAGES=$((PAGES+1))
	done
	SWEPT_SORTED=$(printf '%s' "$SWEPT" | grep -v '^$' | sort)
	SWEPT_UNIQ=$(printf '%s' "$SWEPT_SORTED" | sort -u)
	if [ "$SWEPT_SORTED" = "$SWEPT_UNIQ" ]; then
		_pass "keyset sweep of /shared repeated no row ($PAGES pages)"
	else
		_fail "keyset sweep repeated a row" "$(printf '%s' "$SWEPT_SORTED" | uniq -d | head -3)"
	fi
	if [ "$SWEPT_UNIQ" = "$ALL_HASHES" ]; then
		_pass "keyset sweep of /shared covered every row ($SWEEP_TOTAL rows)"
	else
		_fail "keyset sweep skipped a row" \
			"missing: $(comm -23 <(printf '%s\n' "$ALL_HASHES") <(printf '%s\n' "$SWEPT_UNIQ") | head -3)"
	fi

	# An anchor past the end is an empty page, not an error: that is how a
	# sweep terminates when the last row is deleted mid-walk.
	_curl "${AUTH[@]}" "$HOST/api/v0/shared?sort=hash&after=ffffffffffffffffffffffffffffffff"
	_assert_status 200 "after= past the last row → 200"
	_assert_json_eq '.shared | length' 0 "after= past the last row returns an empty page"

	# Mutable column refused as an anchor, and desc refused outright.
	_curl "${AUTH[@]}" "$HOST/api/v0/shared?sort=name&after=x"
	_assert_status 400 "after= on a mutable sort column → 400"
	_curl "${AUTH[@]}" "$HOST/api/v0/shared?sort=hash&order=desc&after=x"
	_assert_status 400 "after= with order=desc → 400"
else
	echo "    info: nothing shared; keyset sweep skipped"
fi

# --- Numeric identity anchor. /shared above walks a string `hash`; a numeric
# column (index/ecid/search_id) goes through ANCHOR_ON_NUM, which strtoull's the
# token instead. The window arithmetic is the same shared code /shared already
# exercises, so this only has to prove the numeric comparator seeks in the right
# direction. A stock daemon holds only category 0, so seed one row to step past
# and delete it after. Assert the precondition rather than assume it: a failed
# GET yields total="null", which must fail loudly, not skip and pass empty.
_curl "${AUTH[@]}" "$HOST/api/v0/categories?limit=1000000000&sort=index"
BEFORE_IDX=$(printf '%s' "$CURL_BODY" | jq -c '[.categories[].index]')
curl -s -X POST "${AUTH[@]}" -H "Content-Type: application/json" \
	-d '{"name":"phase28-cat","save_path":"/tmp/28-cat-sweep"}' \
	"$HOST/api/v0/categories" > /dev/null 2>&1
_curl "${AUTH[@]}" "$HOST/api/v0/categories?limit=1000000000&sort=index"
_assert_json_ge '.total' 2 '/categories seeded a row to step past'
NEW_IDX=$(printf '%s' "$CURL_BODY" \
	| jq -r --argjson b "$BEFORE_IDX" '[.categories[].index] - $b | first // empty')

# after=0 seeks past category 0 and returns the rest -- a real numeric advance,
# not just a parse edge. A reversed comparator would return nothing here.
_curl "${AUTH[@]}" "$HOST/api/v0/categories?sort=index&after=0"
_assert_status 200 "after=0 (numeric) → 200"
_assert_json_ge ".categories | length" 1 "after=0 returns the rows past index 0"
_assert_json_eq "[.categories[].index] | any(. == 0)" false "after=0 excludes index 0"

# A non-numeric token sorts before everything -> first page (the strtoull-failed
# branch), the same shape as an out-of-range offset.
_curl "${AUTH[@]}" "$HOST/api/v0/categories?sort=index&after=notanumber"
_assert_status 200 "after=<non-numeric> on a numeric anchor → 200 (first page)"
_assert_json_eq ".categories[0].index" 0 "after=<non-numeric> falls back to the first page"

# A parsed token past every index -> empty page, not an error.
_curl "${AUTH[@]}" "$HOST/api/v0/categories?sort=index&after=1000000000"
_assert_status 200 "after= past the last index → 200"
_assert_json_eq ".categories | length" 0 "after= past the last index returns an empty page"

# Delete the seeded row (a single row, so no index-renumber concern).
[ -n "$NEW_IDX" ] && curl -s -X DELETE "${AUTH[@]}" "$HOST/api/v0/categories/$NEW_IDX" > /dev/null 2>&1

curl -s -X DELETE "${AUTH[@]}" "$HOST/api/v0/search/$SEARCH_SID" > /dev/null 2>&1

echo
echo "28-list-pagination-sort: $((TEST_COUNT-FAIL_COUNT))/$TEST_COUNT passed"
[ "$FAIL_COUNT" -eq 0 ] || exit 1
