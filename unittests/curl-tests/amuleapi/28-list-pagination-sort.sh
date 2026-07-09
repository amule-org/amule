#!/usr/bin/env bash
#
# amuleapi list pagination + sorting (issue #357). Exercises the shared
# ?limit/&offset/&sort/&order machinery across every list endpoint:
# /downloads, /shared, /servers, /clients and /search/results.
#
# Data-tolerant like the other read smokes — it asserts the pagination
# metadata (`total`/`offset`/`limit`), that `limit` bounds the array
# length, that omitting params preserves the array, and that malformed
# params are rejected with 400. Ordering correctness needs >= 2 items to
# observe and is left to the live test / unit layer; here `sort` is only
# asserted to be accepted (valid field) or rejected (unknown field).
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

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required. brew install jq."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 28-list-pagination-sort smoke @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for pagination tests"

sleep 3 # let the refresher build its caches

AUTH=(-H "Authorization: Bearer $TOKEN")

# endpoint:array-key pairs. search/results wraps under "results".
ENDPOINTS=(
	"downloads:downloads"
	"shared:shared"
	"servers:servers"
	"clients:clients"
	"search/results:results"
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
	_assert_json_eq ".limit | type"  number "/$ep limit is a number"
	_assert_json_eq ".offset"        0      "/$ep default offset is 0"

	# 2. limit bounds the array length; limit echoes back.
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=1"
	_assert_status 200 "GET /$ep?limit=1 → 200"
	_assert_json_le ".$key | length" 1 "/$ep?limit=1 returns <= 1 item"
	_assert_json_eq ".limit" 1 "/$ep?limit=1 echoes limit=1"

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

	# 7. limit is capped at 500 (echoed limit never exceeds 500).
	_curl "${AUTH[@]}" "$HOST/api/v0/$ep?limit=99999"
	_assert_status 200 "GET /$ep?limit=99999 → 200 (clamped)"
	_assert_json_le ".limit" 500 "/$ep?limit=99999 echoes limit <= 500"
done

echo
echo "28-list-pagination-sort: $((TEST_COUNT-FAIL_COUNT))/$TEST_COUNT passed"
[ "$FAIL_COUNT" -eq 0 ] || exit 1
