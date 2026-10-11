#! /bin/bash
#
# Verifies that each given catalog matches its committed version in content.
# Run it after regenerating the catalogs. Paths are relative to the repository
# root.
#
# xgettext, po4a and msgmerge always rewrite a few volatile header lines and the
# source references, even when no message changed, and a translator's tool may
# re-wrap. Both sides are unwrapped and those lines dropped before comparing.
#
# Exits non-zero and prints the differences when any catalog has drifted.

set -u

cd "$(dirname "$0")/.." || exit 1

normalize() {
	msgcat --no-wrap - \
		| grep -vE '^("(POT-Creation-Date|PO-Revision-Date|Last-Translator|Language-Team)|#: )'
}

REPORT=$(mktemp) || exit 1
trap 'rm -f "${REPORT}"' EXIT

drift=0
for po in "$@"; do
	if ! diff <(git show HEAD:"${po}" | normalize) <(normalize < "${po}") > "${REPORT}"; then
		drift=1
		echo "::error file=${po}::content drift detected"
		echo "----- ${po} -----"
		cat "${REPORT}"
	fi
done
exit "${drift}"
