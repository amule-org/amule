# Shared by run-all.sh and the phases that start their own amuleapi.
#
# Locate the amuleapi binary the sub-instances need. Deliberately NOT falling
# back to PATH: an installed package would silently be tested in place of the
# working tree, which is worse than not finding anything. Set AMULEAPI_BIN to
# point somewhere else on purpose.
#
# Any build*/ directory counts, not a fixed list of three: per-branch and
# per-arch trees are normal, and a name this function has never heard of used
# to mean a silent skip. Where several exist the NEWEST wins -- picking the
# first match let a stale build/ supply the binary while the tree under test
# was built somewhere else, which is the one failure mode that produces a
# confident wrong answer rather than a missing one. Unmatched globs stay
# literal and fail the -x test, so no nullglob is needed.
_find_amuleapi_bin() {
	local root=$1 c best=
	for c in "$root"/src/webapi/amuleapi \
		"$root"/build*/src/webapi/amuleapi \
		"$root"/_build/src/webapi/amuleapi \
		"$root"/cmake-build-*/src/webapi/amuleapi; do
		[ -x "$c" ] || continue
		if [ -z "$best" ] || [ "$c" -nt "$best" ]; then
			best=$c
		fi
	done
	[ -n "$best" ] || return 1
	echo "$best"
}

# Every build tree puts amuled at src/amuled, next to src/webapi/amuleapi.
_find_amuled_bin() { echo "$(dirname "$1")/../amuled"; }

# _wait_health API_ROOT [TRIES]: poll API_ROOT/health every 0.5 s. Fails after
# TRIES polls (default 12).
_wait_health() {
	local _
	for _ in $(seq 1 "${2:-12}"); do
		curl -s -o /dev/null --max-time 1 "$1/health" 2>/dev/null && return 0
		sleep 0.5
	done
	return 1
}

# _seed_amuleapi BIN DIR [ADMIN] [GUEST]: store $EC_PASSWORD, and the role
# passwords given, in DIR's amuleapi.conf (amuleapi has no --password flag).
# The first write creates a default amuleapi.conf when DIR has none.
_seed_amuleapi() {
	local bin=$1 dir=$2
	if [ -n "${3:-}" ]; then
		printf '%s\n' "$3" | "$bin" --config-dir="$dir" --set-admin-pass-stdin >/dev/null || return 1
	fi
	if [ -n "${4:-}" ]; then
		printf '%s\n' "$4" | "$bin" --config-dir="$dir" --set-guest-pass-stdin >/dev/null || return 1
	fi
	printf '%s\n' "$EC_PASSWORD" | "$bin" --config-dir="$dir" --set-ec-pass-stdin >/dev/null
}
