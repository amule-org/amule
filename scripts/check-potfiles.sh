#! /bin/bash
#
# Verifies that po/POTFILES.in lists every source file that marks a string for
# translation.
#
# xgettext is told which files to scan with --files-from=po/POTFILES.in, so a
# file missing from that list is simply never read: its _() strings never reach
# po/amule.pot, never reach Weblate, and render untranslated forever no matter
# how complete a language is. Nothing else catches this. The "catalogs in sync"
# job regenerates the template from the same list, so the check and the bug
# share a blind spot -- the catalogs really are in sync with what was scanned.
#
# Exits non-zero and names the files when the list has drifted.

set -u

cd "$(dirname "$0")/.." || exit 1

POTFILES=po/POTFILES.in

# The keywords xgettext is invoked with; keep in step with update-po.sh.
#
# Matched only where the call opens with a string literal, optionally wrapped
# in wxT(), which is the only form xgettext can extract anyway -- `_(variable)`
# yields nothing whether or not the file is listed. Requiring the literal is
# also what keeps XPM pixmap data out: rows like "... &.&._ _ _ ( F <.-Xj ..."
# contain the keyword next to a paren, but never immediately followed by one
# and a quote.
KEYWORDS='_|wxTRANSLATE|wxPLURAL'
LITERAL='(^|[^A-Za-z0-9_])('"${KEYWORDS}"')\([[:space:]]*(wxT\()?"'

status=0

# Files that mark strings but are not listed.
while read -r file; do
	case "${file}" in
	# The amuleapi Web UI ships its own JSON dictionaries and has its own
	# gate (src/webapi/tools/check-i18n.mjs); its C++ side serves that UI
	# rather than emitting user-facing text through gettext.
	src/webapi/*) continue ;;
	unittests/*) continue ;;
	esac

	grep -qxF "${file}" "${POTFILES}" && continue

	if [[ ${status} == 0 ]]; then
		echo "Source files marking translatable strings but missing from ${POTFILES}:" >&2
	fi
	echo "  ${file}" >&2
	status=1
done < <(git grep -lE "${LITERAL}" -- '*.c' '*.cpp' '*.h' | sort)

if [[ ${status} != 0 ]]; then
	echo >&2
	echo "Add them to ${POTFILES}, then run scripts/update-po.sh and commit the result." >&2
fi

# The reverse: a listed file that no longer exists. xgettext fails on this
# anyway, but failing here says which line to delete instead of dying inside
# a regeneration.
while read -r listed; do
	[[ -z ${listed} || ${listed} == \#* ]] && continue
	[[ -f ${listed} ]] && continue
	echo "${POTFILES} lists a file that does not exist: ${listed}" >&2
	status=1
done < "${POTFILES}"

exit "${status}"
