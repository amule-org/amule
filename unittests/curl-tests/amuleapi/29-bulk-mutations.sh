#!/usr/bin/env bash
#
# amuleapi bulk mutations (issue #358). Exercises the unified per-item
# `results` envelope across POST /downloads, PATCH/DELETE /downloads and
# PATCH /shared, plus the 207 Multi-Status / 400 conventions.
#
# Data-tolerant: a nonexistent hash yields a per-item `not_found`, so the
# full path (routing -> parse -> per-hash loop -> per-item error -> 207
# aggregation) is exercised without needing real downloads/shares on the
# daemon. Success-path (ok:true) needs live files and is left to manual /
# integration testing.
#
# Bring-up: see run-all.sh / 04-read-downloads-shared.sh.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0
BODY=$(mktemp -t amuleapi_29_bulk_body.XXXXXX)
trap 'rm -f "$BODY"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() { TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1)); echo "  FAIL  $1"; shift; for a in "$@"; do echo "        $a"; done; }

# _req METHOD PATH JSON  -> sets STATUS + CURL_BODY
_req() {
	local method=$1 path=$2 json=${3:-}
	local args=(-s --max-time 10 -o "$BODY" -w '%{http_code}' -X "$method"
		-H "Authorization: Bearer $TOKEN")
	[ -n "$json" ] && args+=(-H "Content-Type: application/json" -d "$json")
	STATUS=$(curl "${args[@]}" "$HOST$path") || _die "curl failed: $method $path"
	CURL_BODY=$(cat "$BODY")
}

_status() { [ "$STATUS" = "$1" ] && _pass "$2 (HTTP $STATUS)" || _fail "$2" "want HTTP $1 got $STATUS" "body: $(printf %s "$CURL_BODY" | head -c 200)"; }
_jq()     { local a; a=$(printf %s "$CURL_BODY" | jq -r "$1" 2>/dev/null); [ "$a" = "$2" ] && _pass "$3" || _fail "$3" "want '$2' got '$a'" "body: $CURL_BODY"; }

command -v jq >/dev/null 2>&1 || _die "jq required"
curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" || _die "amuleapi at $HOST not reachable"

echo "amuleapi 29-bulk-mutations @ $HOST"
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"
sleep 2

H0="00000000000000000000000000000000"
H1="11111111111111111111111111111111"

# --- auth gate --------------------------------------------------------
S=$(curl -s -o /dev/null -w '%{http_code}' -X PATCH -H "Content-Type: application/json" \
	-d "{\"hashes\":[\"$H0\"],\"priority\":\"high\"}" "$HOST/api/v0/shared")
[ "$S" = 401 ] && _pass "PATCH /shared without creds -> 401" || _fail "PATCH /shared no-creds" "want 401 got $S"

# --- per-item not_found => 207 Multi-Status ---------------------------
echo "  --- 207 per-item not_found ---"
_req PATCH /api/v0/downloads "{\"hashes\":[\"$H0\"],\"priority\":\"high\"}"
_status 207 "PATCH /downloads [bogus hash]"
_jq '.results | length' 1                "  results has 1 entry"
_jq '.results[0].id'    "$H0"            "  results[0].id echoes the hash"
_jq '.results[0].ok'    false            "  results[0].ok is false"
_jq '.results[0].error.code' not_found   "  results[0].error.code is not_found"

_req DELETE /api/v0/downloads "{\"hashes\":[\"$H0\"]}"
_status 207 "DELETE /downloads [bogus hash]"
_jq '.results[0].error.code' not_found   "  DELETE results[0] not_found"

_req PATCH /api/v0/shared "{\"hashes\":[\"$H0\"],\"priority\":\"high\"}"
_status 207 "PATCH /shared [bogus hash]"
_jq '.results[0].error.code' not_found   "  PATCH /shared results[0] not_found"

# --- multiple items => length matches --------------------------------
_req PATCH /api/v0/shared "{\"hashes\":[\"$H0\",\"$H1\"],\"priority\":\"low\"}"
_status 207 "PATCH /shared [2 bogus hashes]"
_jq '.results | length' 2                "  results has 2 entries"
_jq '[.results[].ok] | any'  false       "  no item succeeded"

# --- bad requests => 400 ---------------------------------------------
echo "  --- 400 validation ---"
_req PATCH /api/v0/downloads "{\"priority\":\"high\"}";            _status 400 "PATCH /downloads missing hashes"
_req PATCH /api/v0/downloads "{\"hashes\":[]}";                    _status 400 "PATCH /downloads empty hashes"
_req PATCH /api/v0/downloads "{\"hashes\":[\"$H0\"]}";            _status 400 "PATCH /downloads no patch fields"
_req PATCH /api/v0/downloads "{\"hashes\":[\"$H0\"],\"priority\":\"bogus\"}"; _status 400 "PATCH /downloads bad priority"
_req DELETE /api/v0/downloads "{}";                               _status 400 "DELETE /downloads missing hashes"
_req PATCH /api/v0/shared "{\"hashes\":[\"$H0\"]}";              _status 400 "PATCH /shared missing priority"
_req PATCH /api/v0/shared "{\"hashes\":[\"$H0\"],\"priority\":\"nope\"}";     _status 400 "PATCH /shared bad priority"

# --- method routing ---------------------------------------------------
_req PUT /api/v0/downloads "{}";  _status 405 "PUT /downloads -> 405"
_req PUT /api/v0/shared "{}";     _status 405 "PUT /shared -> 405"

# --- POST /downloads unified shape (no legacy counters) --------------
echo "  --- POST /downloads unified results shape ---"
LINK="ed2k://|file|bulk-test.bin|1024|0123456789ABCDEF0123456789ABCDEF|/"
_req POST /api/v0/downloads "{\"links\":[\"$LINK\"]}"
_jq '.results | type' array              "POST /downloads has results[] array"
_jq '.results | length' 1                "  one result entry"
_jq '.results[0].id' "$LINK"             "  result id echoes the link"
_jq '.accepted // "absent"' absent       "  legacy 'accepted' counter removed"
_jq '.ok // "absent"' absent             "  legacy 'ok' bool removed"

echo
echo "29-bulk-mutations: $((TEST_COUNT-FAIL_COUNT))/$TEST_COUNT passed"
[ "$FAIL_COUNT" -eq 0 ] || exit 1
