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

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

# _req METHOD PATH JSON
_req() {
	local method=$1 path=$2 json=${3:-}
	local args=(-X "$method" -H "Authorization: Bearer $TOKEN")
	[ -n "$json" ] && args+=(-H "Content-Type: application/json" -d "$json")
	_curl "${args[@]}" "$HOST$path"
}

_preflight

echo "amuleapi 29-bulk-mutations @ $HOST"
TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] || _die "login failed"
sleep 2

H0="00000000000000000000000000000000"
H1="11111111111111111111111111111111"

# --- auth gate --------------------------------------------------------
S=$(curl -s -o /dev/null -w '%{http_code}' -X PATCH -H "Content-Type: application/json" \
	-d "{\"hashes\":[\"$H0\"],\"priority\":\"high\"}" "$API/shared")
[ "$S" = 401 ] && _pass "PATCH /shared without creds -> 401" || _fail "PATCH /shared no-creds" "want 401 got $S"

# --- per-item not_found => 207 Multi-Status ---------------------------
echo "  --- 207 per-item not_found ---"
_req PATCH /api/v1/downloads "{\"hashes\":[\"$H0\"],\"priority\":\"high\"}"
_assert_status 207 "PATCH /downloads [bogus hash]"
_assert_json_eq '.results | length' 1                "  results has 1 entry"
_assert_json_eq '.results[0].id'    "$H0"            "  results[0].id echoes the hash"
_assert_json_eq '.results[0].ok'    false            "  results[0].ok is false"
_assert_json_eq '.results[0].error.code' not_found   "  results[0].error.code is not_found"

# action:"stop" is a recognized bulk value — it reaches the per-hash
# loop (bogus hash → not_found), proving the parse accepts it, unlike a
# bad enum (see the 400 section).
_req PATCH /api/v1/downloads "{\"hashes\":[\"$H0\"],\"action\":\"stop\"}"
_assert_status 207 "PATCH /downloads action=stop [bogus hash]"
_assert_json_eq '.results[0].error.code' not_found   "  bulk action=stop results[0] not_found (value accepted)"

_req DELETE /api/v1/downloads "{\"hashes\":[\"$H0\"]}"
_assert_status 207 "DELETE /downloads [bogus hash]"
_assert_json_eq '.results[0].error.code' not_found   "  DELETE results[0] not_found"

_req PATCH /api/v1/shared "{\"hashes\":[\"$H0\"],\"priority\":\"high\"}"
_assert_status 207 "PATCH /shared [bogus hash]"
_assert_json_eq '.results[0].error.code' not_found   "  PATCH /shared results[0] not_found"

# --- multiple items => length matches --------------------------------
_req PATCH /api/v1/shared "{\"hashes\":[\"$H0\",\"$H1\"],\"priority\":\"low\"}"
_assert_status 207 "PATCH /shared [2 bogus hashes]"
_assert_json_eq '.results | length' 2                "  results has 2 entries"
_assert_json_eq '[.results[].ok] | any'  false       "  no item succeeded"

# --- bad requests => 400 ---------------------------------------------
echo "  --- 400 validation ---"
_req PATCH /api/v1/downloads "{\"priority\":\"high\"}";            _assert_status 400 "PATCH /downloads missing hashes"
_req PATCH /api/v1/downloads "{\"hashes\":[]}";                    _assert_status 400 "PATCH /downloads empty hashes"
_req PATCH /api/v1/downloads "{\"hashes\":[\"$H0\"]}";            _assert_status 400 "PATCH /downloads no patch fields"
_req PATCH /api/v1/downloads "{\"hashes\":[\"$H0\"],\"priority\":\"bogus\"}"; _assert_status 400 "PATCH /downloads bad priority"
_req PATCH /api/v1/downloads "{\"hashes\":[\"$H0\"],\"status\":\"bogus\"}";   _assert_status 400 "PATCH /downloads bad status"
_req DELETE /api/v1/downloads "{}";                               _assert_status 400 "DELETE /downloads missing hashes"
_req PATCH /api/v1/shared "{\"hashes\":[\"$H0\"]}";              _assert_status 400 "PATCH /shared missing priority"
_req PATCH /api/v1/shared "{\"hashes\":[\"$H0\"],\"priority\":\"nope\"}";     _assert_status 400 "PATCH /shared bad priority"

# --- method routing ---------------------------------------------------
_req PUT /api/v1/downloads "{}";  _assert_status 405 "PUT /downloads -> 405"
_req PUT /api/v1/shared "{}";     _assert_status 405 "PUT /shared -> 405"

# --- POST /downloads results shape ----------------------------------
echo "  --- POST /downloads unified results shape ---"
LINK="ed2k://|file|bulk-test.bin|1024|0123456789ABCDEF0123456789ABCDEF|/"
_req POST /api/v1/downloads "{\"links\":[\"$LINK\"]}"
_assert_json_eq '.results | type' array              "POST /downloads has results[] array"
_assert_json_eq '.results | length' 1                "  one result entry"
_assert_json_eq '.results[0].id' "$LINK"             "  result id echoes the link"
_assert_json_eq '.accepted // "absent"' absent       "  no 'accepted' counter"
_assert_json_eq '.ok // "absent"' absent             "  no 'ok' bool"

_summary
