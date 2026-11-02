#!/usr/bin/env bash
#
# Daemon-side checks for run-live.sh, run through run-all.sh so it talks to
# a freshly started amuleapi like any phase. Asserts nothing about amuleapi
# itself: each check is a fixture a live phase needs.

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

_preflight

TOKEN=$(_login "$ADMIN_PASS")
[ -n "$TOKEN" ] || _die "admin login failed"
CURL_AUTO_AUTH=1
AUTH=(-H "Authorization: Bearer $TOKEN")

_canon() { (cd "$1" 2>/dev/null && pwd -P); }

# --- Networks. 19 searches both; 00 needs sources for 04, 33 and 43. ---
_curl "$API/status"
_assert_json_eq '.ed2k.state' connected 'ed2k is connected'
_assert_json_eq '.ed2k.high_id' true 'ed2k has a HighID'
_assert_json_eq '.kad.state' connected 'Kad is connected'
[ "$(_jq '.kad.firewalled_tcp')" = "true" ] &&
	echo "  note  Kad is firewalled; searches still work, inbound Kad contacts do not"

_curl "$API/servers"
_assert_json_eq '(.servers | length > 0)' true 'the server list is not empty (26)'

# --- Allowed roots. 18 and 31 create folders under /tmp and $TMPDIR, the
# shared-file phases plant fixtures in AMULE_SHARED_DIR, and 15 needs some
# folder outside the roots. ---
_curl "$API/preferences"
INCOMING=$(_jq '.directories.incoming_path')
ROOTS=()
while IFS= read -r r; do
	ROOTS+=("$(_canon "$r" || printf '%s' "$r")")
done < <(_jq '.directories.allowed_remote_roots[]')

_covered() {
	local p r
	p=$(_canon "$1") || return 1
	for r in "${ROOTS[@]+"${ROOTS[@]}"}"; do
		case "$p/" in "${r%/}/"*) return 0 ;; esac
	done
	return 1
}

_assert_eq 0 "$(printf '%s\n' "${ROOTS[@]+"${ROOTS[@]}"}" | grep -cx /)" 'allowed roots do not include "/" (15)'
for dir in /tmp "${TMPDIR:-/tmp}" "$AMULE_SHARED_DIR"; do
	if _covered "$dir"; then
		_pass "allowed roots cover $dir"
	else
		_fail "allowed roots cover $dir" "add it to $AMULE_CONFIG_DIR/allowed-remote-dirs.dat"
	fi
done

# --- AMULE_SHARED_DIR is shared. Incoming always is; otherwise a share root
# must cover it. ---
SHARED=$(_canon "$AMULE_SHARED_DIR")
IS_SHARED=0
[ "$SHARED" = "$(_canon "$INCOMING")" ] && IS_SHARED=1
_curl "$API/share_directories"
while IFS=$'\t' read -r path recursive; do
	root=$(_canon "$path") || continue
	if [ "$SHARED" = "$root" ] ||
		{ [ "$recursive" = "true" ] && case "$SHARED/" in "$root/"*) true ;; *) false ;; esac; }; then
		IS_SHARED=1
	fi
done < <(_jq '.directories[] | [.path, (.recursive | tostring)] | @tsv')
if [ "$IS_SHARED" = "1" ]; then
	_pass "amuled shares AMULE_SHARED_DIR"
else
	_fail "amuled shares AMULE_SHARED_DIR" "$AMULE_SHARED_DIR is neither Incoming ($INCOMING) nor under a share root"
fi

_summary
