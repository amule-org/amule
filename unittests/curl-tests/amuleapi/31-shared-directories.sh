#!/usr/bin/env bash
#
# amuleapi 31-shared-directories — the configured share roots.
#
# Endpoints:
#   GET    /api/v0/shared/directories            → {"directories":[{path,recursive}]}
#   PUT    /api/v0/shared/directories            → replace the whole set
#   POST   /api/v0/shared/directories            → add one (idempotent)
#   DELETE /api/v0/shared/directories?path=...   → remove one
#
# These are the roots amuled is configured with, as opposed to /shared which
# lists the files those roots produced. A recursive root is a single entry here
# however many subdirectories it covers.
#
# The write verbs really do mutate the core's configuration, so this smoke
# snapshots the current roots up front and restores them via PUT at the end
# (including on early exit). Everything it adds lives under a mktemp directory,
# never the operator's real shares.
#
# amuled validates paths server-side — a REST client cannot stat the core's
# filesystem — and reports refusals per path in `rejected` while still applying
# the entries that passed. Both halves of that contract are asserted.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_31_shared_dirs_body.XXXXXX)
SCRATCH_DIR=$(mktemp -d -t amuleapi_31_shared_dirs.XXXXXX)
ORIGINAL_DIRS=""

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

# Put the operator's configuration back exactly as we found it, whatever
# happened in between. Registered before the first mutation.
_restore() {
	if [ -n "$ORIGINAL_DIRS" ]; then
		curl -s -o /dev/null --max-time 10 -X PUT \
			-H "Authorization: Bearer ${ADMIN_TOKEN:-}" \
			-H "Content-Type: application/json" \
			-d "{\"directories\":$ORIGINAL_DIRS}" \
			"$HOST/api/v0/shared/directories" 2>/dev/null || true
	fi
	rm -f "$CURL_BODY_FILE"
	rm -rf "$SCRATCH_DIR"
}
trap _restore EXIT

if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 31-shared-directories smoke @ $HOST"

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

# --- 1. Auth + method gates. ----------------------------------------
_curl "$HOST/api/v0/shared/directories"
_assert_status 401 "GET /shared/directories (no token) → 401"

_curl -X PUT -H "Content-Type: application/json" \
	-d '{"directories":[]}' "$HOST/api/v0/shared/directories"
_assert_status 401 "PUT /shared/directories (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	# Reading the roots is GUEST, matching GET /shared and GET /preferences.
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/shared/directories"
	_assert_status 200 "GET /shared/directories (guest) → 200"

	_curl -X PUT -H "Authorization: Bearer $GUEST_TOKEN" -H "Content-Type: application/json" \
		-d '{"directories":[]}' "$HOST/api/v0/shared/directories"
	_assert_status 403 "PUT /shared/directories (guest) → 403"

	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" -H "Content-Type: application/json" \
		-d "{\"path\":\"$SCRATCH_DIR\"}" "$HOST/api/v0/shared/directories"
	_assert_status 403 "POST /shared/directories (guest) → 403"

	_curl -X DELETE -H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/shared/directories?path=%2Ftmp"
	_assert_status 403 "DELETE /shared/directories (guest) → 403"
fi

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_status 405 "PATCH /shared/directories → 405"

# --- 2. Read the current configuration (and snapshot it). -----------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_status 200 "GET /shared/directories → 200"
_assert_json_eq '.directories | type' 'array' "directories is an array"
ORIGINAL_DIRS=$(printf '%s' "$CURL_BODY" | jq -c '.directories')
[ -n "$ORIGINAL_DIRS" ] || _die "could not snapshot the current shared directories"

# --- 3. Body validation. --------------------------------------------
_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d 'not json' "$HOST/api/v0/shared/directories"
_assert_status 400 "PUT (malformed JSON) → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/shared/directories"
_assert_status 400 "PUT (no directories array) → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"directories":[{"path":""}]}' "$HOST/api/v0/shared/directories"
_assert_status 400 "PUT (empty path) → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"directories\":[{\"path\":\"$SCRATCH_DIR\",\"recursive\":\"yes\"}]}" \
	"$HOST/api/v0/shared/directories"
_assert_status 400 "PUT (non-boolean recursive) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/shared/directories"
_assert_status 400 "POST (no path) → 400"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_status 400 "DELETE (no path parameter) → 400"

# --- 4. Add a real directory, then read it back. ---------------------
SCRATCH_ENC=$(jq -rn --arg p "$SCRATCH_DIR" '$p|@uri')

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"path\":\"$SCRATCH_DIR\",\"recursive\":true}" "$HOST/api/v0/shared/directories"
_assert_status 200 "POST (add scratch dir, recursive) → 200"
_assert_json_eq '.ok' 'true' "POST reports ok"
_assert_json_eq '.rejected | length' '0' "POST rejected nothing"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '1' \
	"added directory is listed"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")][0].recursive" 'true' \
	"added directory kept its recursive flag"

# Idempotent re-add: no duplicate, and the flag follows the new value.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"path\":\"$SCRATCH_DIR\",\"recursive\":false}" "$HOST/api/v0/shared/directories"
_assert_status 200 "POST (re-add same path) → 200"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '1' \
	"re-add did not duplicate the entry"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")][0].recursive" 'false' \
	"re-add updated the recursive flag"

# --- 5. Server-side validation of a path the client cannot check. ----
BOGUS="$SCRATCH_DIR/definitely-not-here"
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"path\":\"$BOGUS\"}" "$HOST/api/v0/shared/directories"
_assert_status 200 "POST (nonexistent path) → 200 with a rejection"
_assert_json_eq \
	"[.rejected[] | select(.path==\"$BOGUS\")][0].reason" 'not_found' \
	"nonexistent path rejected as not_found"

# The valid entry must survive a sibling's rejection.
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '1' \
	"rejection did not discard the valid entry"
_assert_json_eq \
	"[.directories[] | select(.path==\"$BOGUS\")] | length" '0' \
	"rejected path was not stored"

# --- 6. Remove it again. --------------------------------------------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/directories?path=$SCRATCH_ENC"
_assert_status 200 "DELETE (configured path) → 200"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$HOST/api/v0/shared/directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '0' \
	"deleted directory is gone"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/shared/directories?path=$SCRATCH_ENC"
_assert_status 404 "DELETE (path not configured) → 404"

# --- Summary. -------------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
