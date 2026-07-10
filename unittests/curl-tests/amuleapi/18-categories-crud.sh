#!/usr/bin/env bash
#
# amuleapi 18-categories-crud — categories CRUD.
#
# Endpoints:
#   POST   /api/v0/categories             — create
#       body: {name, path?, comment?, color?, priority?}
#   PATCH  /api/v0/categories/{index}     — update
#       body: any subset of {name, path, comment, color, priority}
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

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

mkdir -p "$TEST_PATH"

echo "amuleapi 18-categories-crud smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

sleep 4

# --- 1. Auth + admin gate. -----------------------------------------
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
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"name\":\"$TEST_NAME\",\"path\":\"$TEST_PATH\",\"comment\":\"18-categories-crud test\",\"priority\":\"high\"}" \
	"$HOST/api/v0/categories"
_assert_status 201 "POST /categories (create) → 201"
_assert_json_eq '.ok'   true        'create response.ok==true'
_assert_json_eq '.name' "$TEST_NAME" 'create response echoes name'

NEW_IDX=$(printf '%s' "$CURL_BODY" | jq -r '.index')
if [ -n "$NEW_IDX" ] && [ "$NEW_IDX" != "null" ]; then
	_pass "create response includes assigned index ($NEW_IDX)"
else
	_die "could not parse new category index from create response"
fi

# Verify by GET /categories.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/categories"
LOOKUP=$(printf '%s' "$CURL_BODY" \
	| jq --arg n "$TEST_NAME" \
	  '[.categories[] | select(.name == $n)] | first')
if [ "$LOOKUP" != "null" ]; then
	_pass "Created category surfaces in /categories list"
else
	_fail "Created category lookup" \
		"could not find $TEST_NAME in /categories list"
fi

# --- 3. POST error paths. -----------------------------------------
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/categories"
_assert_status 400 "POST /categories (no name) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"name\":\"x\",\"priority\":\"bogus\"}" "$HOST/api/v0/categories"
_assert_status 400 "POST /categories (bad priority enum) → 400"

# A category priority is applied to its files as a DOWNLOAD priority, so it
# takes the restricted download set (low/normal/high/auto). very_low and
# release are downloads-invalid — the daemon clamps them to Normal on the
# next restart — so they must be rejected here too (issue #384).
for p in very_low release; do
	_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" \
		-H "Content-Type: application/json" \
		-d "{\"name\":\"x\",\"priority\":\"$p\"}" "$HOST/api/v0/categories"
	_assert_status 400 "POST /categories (priority=$p rejected) → 400"
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
	_assert_status 400 "PATCH /categories (priority=$p rejected) → 400"
done

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"name":"unknown"}' "$HOST/api/v0/categories/199"
_assert_status 404 "PATCH /categories unknown index → 404"

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" \
	-H "Content-Type: application/json" \
	-d '{"name":"x"}' "$HOST/api/v0/categories/not-a-number"
_assert_status 400 "PATCH /categories non-numeric index → 400"

# --- 6. DELETE happy path + cannot-delete-default + no-stale. ----
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/categories/0"
_assert_status 400 "DELETE /categories/0 (default) → 400"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/categories/$NEW_IDX"
_assert_status 200 "DELETE /categories/$NEW_IDX → 200"
_assert_json_eq '.ok'    true       'DELETE response.ok==true'
_assert_json_eq '.index' "$NEW_IDX" 'DELETE response echoes index'

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

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
