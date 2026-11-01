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
