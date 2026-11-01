#!/usr/bin/env bash
#
# amuleapi 31-shared-directories — the configured share roots.
#
# Endpoints:
#   GET    /api/v1/share_directories            → {"directories":[{path,recursive}]}
#   PUT    /api/v1/share_directories            → replace the whole set
#   POST   /api/v1/share_directories            → add one (idempotent)
#   DELETE /api/v1/share_directories?path=...   → remove one
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

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

SCRATCH_DIR=$(mktemp -d -t amuleapi_31_shared_dirs.XXXXXX)
ORIGINAL_DIRS=""

# Put the operator's configuration back exactly as we found it, whatever
# happened in between. Registered before the first mutation.
_restore() {
	if [ -n "$ORIGINAL_DIRS" ]; then
		curl -s -o /dev/null --max-time 10 -X PUT \
			-H "Authorization: Bearer ${ADMIN_TOKEN:-}" \
			-H "Content-Type: application/json" \
			-d "{\"directories\":$ORIGINAL_DIRS}" \
			"$API/share_directories" 2>/dev/null || true
	fi
	rm -rf "$SCRATCH_DIR"
}
_on_exit _restore

_preflight

echo "amuleapi 31-shared-directories smoke @ $HOST"

ADMIN_TOKEN=$(_login "$ADMIN_PASS")
[ -n "$ADMIN_TOKEN" ] || _die "admin login failed"

GUEST_TOKEN=$(_login "$GUEST_PASS")
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && HAVE_GUEST=1

# --- 1. Auth + method gates. ----------------------------------------
_curl "$API/share_directories"
_assert_status 401 "GET /share_directories (no token) → 401"

_curl -X PUT -H "Content-Type: application/json" \
	-d '{"directories":[]}' "$API/share_directories"
_assert_status 401 "PUT /share_directories (no token) → 401"

if [ "$HAVE_GUEST" = "1" ]; then
	# Reading the roots is GUEST, matching GET /shared and GET /preferences.
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$API/share_directories"
	_assert_status 200 "GET /share_directories (guest) → 200"

	_curl -X PUT -H "Authorization: Bearer $GUEST_TOKEN" -H "Content-Type: application/json" \
		-d '{"directories":[]}' "$API/share_directories"
	_assert_status 403 "PUT /share_directories (guest) → 403"

	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" -H "Content-Type: application/json" \
		-d "{\"path\":\"$SCRATCH_DIR\"}" "$API/share_directories"
	_assert_status 403 "POST /share_directories (guest) → 403"

	_curl -X DELETE -H "Authorization: Bearer $GUEST_TOKEN" \
		"$API/share_directories?path=%2Ftmp"
	_assert_status 403 "DELETE /share_directories (guest) → 403"
fi

_curl -X PATCH -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_status 405 "PATCH /share_directories → 405"

# --- 2. Read the current configuration (and snapshot it). -----------
_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_status 200 "GET /share_directories → 200"
_assert_json_eq '.directories | type' 'array' "directories is an array"
ORIGINAL_DIRS=$(printf '%s' "$CURL_BODY" | jq -c '.directories')
[ -n "$ORIGINAL_DIRS" ] || _die "could not snapshot the current shared directories"

# Re-PUT the daemon's current roots unchanged. Only a *newly added* folder is checked against
# the allowed roots, so an already-configured one is re-accepted without a check -- re-sending
# the whole list must never drop a legacy share that now sits outside the roots (or whose disk
# is unmounted). So this round-trip has no rejections whatever the daemon's allowed-roots gate.
_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"directories\":$ORIGINAL_DIRS}" "$API/share_directories"
_assert_status 200 "PUT the current roots unchanged → 200 (already-shared entries are not re-checked)"
_assert_json_eq '[.results[] | select(.ok==false)] | length' '0' \
	"re-sending the existing roots rejects nothing, even any outside the allowed roots"

# --- 3. Body validation. --------------------------------------------
_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d 'not json' "$API/share_directories"
_assert_status 400 "PUT (malformed JSON) → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{}' "$API/share_directories"
_assert_status 400 "PUT (no directories array) → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{"directories":[{"path":""}]}' "$API/share_directories"
_assert_status 400 "PUT (empty path) → 400"

_curl -X PUT -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"directories\":[{\"path\":\"$SCRATCH_DIR\",\"recursive\":\"yes\"}]}" \
	"$API/share_directories"
_assert_status 400 "PUT (non-boolean recursive) → 400"

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d '{}' "$API/share_directories"
_assert_status 400 "POST (no path) → 400"

_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_status 400 "DELETE (no path parameter) → 400"

# --- 4. Add a real directory, then read it back. ---------------------
#
# `real` means real to amuled: it stats the path itself, so the mktemp directory
# above exists for the daemon only when the daemon shares this filesystem.
# Against a containerised or remote one everything below is rejected as
# not_found, so probe once and say so instead of failing seven assertions.
#
# not_readable counts as the same setup problem. The core distinguishes the
# two (EC_SHAREDDIR_ERR_*: 1 = missing or not a directory, 2 = unreadable),
# and a same-host daemon running as another user hits the second one on the
# 0700 directory mktemp -d just created. Either way the daemon cannot take
# this path, which is the condition the assertions below depend on.
SCRATCH_ENC=$(jq -rn --arg p "$SCRATCH_DIR" '$p|@uri')

_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"path\":\"$SCRATCH_DIR\",\"recursive\":true}" "$API/share_directories"
SCRATCH_REJECTION=$(printf '%s' "$CURL_BODY" |
	jq -r "[.results[] | select(.id==\"$SCRATCH_DIR\" and .ok==false)][0].error.code" 2>/dev/null)
SHARE_FS_VISIBLE=1
case "$SCRATCH_REJECTION" in
not_found | not_readable)
	SHARE_FS_VISIBLE=0
	echo "  SKIP  the assertions that need amuled to take $SCRATCH_DIR ($SCRATCH_REJECTION)"
	echo "        (it stats paths itself; run this against a daemon on this host"
	echo "         and as a user that can read the path, or set the scratch path"
	echo "         to one the daemon can reach)"
	;;
path_not_allowed)
	# The daemon can see the directory but its allowed-roots gate refuses it: either the
	# out-of-the-box empty gate (which refuses every remote folder change) or a configured
	# gate that does not cover the temp dir. That is the EC path-hardening refusal, so assert
	# its contract here -- it is per-item in the 207 envelope, like not_found/not_readable --
	# and skip the positive add/remove assertions, which need a path the daemon will accept.
	SHARE_FS_VISIBLE=0
	_assert_status 207 "POST (scratch dir outside allowed roots) → 207 with a rejection"
	_assert_json_eq \
		"[.results[] | select(.id==\"$SCRATCH_DIR\")][0].error.code" 'path_not_allowed' \
		"a folder outside the allowed roots is rejected as path_not_allowed"
	_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
	_assert_json_eq \
		"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '0' \
		"the refused folder was not stored"
	echo "  SKIP  the positive add/remove assertions ($SCRATCH_DIR is outside the daemon's"
	echo "        allowed roots; add it to allowed-remote-dirs.dat on the daemon to exercise them)"
	;;
esac

if [ "$SHARE_FS_VISIBLE" -eq 1 ]; then
_assert_status 200 "POST (add scratch dir, recursive) → 200"
_assert_json_eq '.results | type' array "POST returns a results array"
_assert_json_eq '[.results[] | select(.ok==false)] | length' '0' "POST rejected nothing"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '1' \
	"added directory is listed"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")][0].recursive" 'true' \
	"added directory kept its recursive flag"

# Idempotent re-add: no duplicate, and the flag follows the new value.
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"path\":\"$SCRATCH_DIR\",\"recursive\":false}" "$API/share_directories"
_assert_status 200 "POST (re-add same path) → 200"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '1' \
	"re-add did not duplicate the entry"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")][0].recursive" 'false' \
	"re-add updated the recursive flag"
fi # SHARE_FS_VISIBLE -- section 4

# --- 5. Server-side validation of a path the client cannot check. ----
# The bogus-path half does not need a visible scratch dir: a path that exists
# nowhere is rejected either way.
BOGUS="$SCRATCH_DIR/definitely-not-here"
_curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
	-d "{\"path\":\"$BOGUS\"}" "$API/share_directories"
# A rejection makes the whole response 207, as with every other multi-item
# mutation. It was 200, which meant a caller had to read the body to learn that
# part of the request had not applied.
_assert_status 207 "POST (nonexistent path) → 207 with a rejection"
_assert_json_eq \
	"[.results[] | select(.id==\"$BOGUS\")][0].error.code" 'not_found' \
	"nonexistent path rejected as not_found"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$BOGUS\")] | length" '0' \
	"rejected path was not stored"
if [ "$SHARE_FS_VISIBLE" -eq 1 ]; then
# The valid entry must survive a sibling's rejection.
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '1' \
	"rejection did not discard the valid entry"

# --- 6. Remove it again. --------------------------------------------
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/share_directories?path=$SCRATCH_ENC"
_assert_status 200 "DELETE (configured path) → 200"

_curl -H "Authorization: Bearer $ADMIN_TOKEN" "$API/share_directories"
_assert_json_eq \
	"[.directories[] | select(.path==\"$SCRATCH_DIR\")] | length" '0' \
	"deleted directory is gone"
fi # SHARE_FS_VISIBLE -- sections 5b and 6

# Never configured, whether or not the scratch dir ever was.
_curl -X DELETE -H "Authorization: Bearer $ADMIN_TOKEN" \
	"$API/share_directories?path=$SCRATCH_ENC"
_assert_status 404 "DELETE (path not configured) → 404"

# --- Summary. -------------------------------------------------------
[ "$SHARE_FS_VISIBLE" -eq 0 ] && SKIP_NOTE=" (local-filesystem share assertions skipped)"
_summary
