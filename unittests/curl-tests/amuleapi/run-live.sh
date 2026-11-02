#!/usr/bin/env bash
#
# Runs the amuleapi curl suite against a live, connected amuled, after
# checking every fixture the network-dependent phases need. CI reaches only
# the offline checks; this is the run that covers the rest. It refuses to
# start while anything is missing, so a wrong setup cannot pass as SKIPs.
#
# Required environment:
#   AMULE_CONFIG_DIR  config dir of the running amuled. Use a copy, not your
#                     own: the phases change servers, shares, preferences and
#                     downloads, and 00-peer-fixture downloads a large file.
#   AMULE_SHARED_DIR  a writable directory that amuled shares (its Incoming)
#
# The daemon must be connected to ed2k with a HighID and to Kad, list at
# least one server, and its allowed-remote-dirs.dat must cover /tmp,
# ${TMPDIR:-/tmp} and AMULE_SHARED_DIR without being "/".
#
# Usage:
#   AMULE_CONFIG_DIR=... AMULE_SHARED_DIR=... ./run-live.sh [phase...]

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT="${AMULEAPI_ROOT:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
. "$SCRIPT_DIR/lib/amuleapi-bin.sh"

export AMULEAPI_BIN="${AMULEAPI_BIN:-$(_find_amuleapi_bin "$ROOT")}"
export AMULED_BIN="${AMULED_BIN:-$(dirname "$AMULEAPI_BIN")/../amuled}"
export EC_PORT=${EC_PORT:-4712}

PROBLEMS=()
_problem() { PROBLEMS+=("$1"); }

for tool in curl jq python3; do
	command -v "$tool" >/dev/null 2>&1 || _problem "$tool is not installed"
done
# 45 and 49 drive EC from Python and skip without it.
python3 -c 'import cryptography' 2>/dev/null ||
	_problem "python3 has no cryptography module (use a venv with it on PATH)"
[ -x "$AMULEAPI_BIN" ] || _problem "no amuleapi binary (set AMULEAPI_BIN)"
# 45 and 49 start their own offline amuled.
[ -x "$AMULED_BIN" ] || _problem "no amuled binary next to amuleapi (set AMULED_BIN)"

if [ -z "${AMULE_CONFIG_DIR:-}" ]; then
	_problem "AMULE_CONFIG_DIR is unset"
elif [ ! -f "$AMULE_CONFIG_DIR/amule.conf" ]; then
	_problem "AMULE_CONFIG_DIR=$AMULE_CONFIG_DIR has no amule.conf"
else
	case "$(cd "$AMULE_CONFIG_DIR" && pwd -P)" in
	"$HOME/.aMule" | "$HOME/Library/Application Support/aMule")
		_problem "AMULE_CONFIG_DIR is your own config dir; run against a copy" ;;
	esac
	CONF_EC_PORT=$(sed -n 's/^ECPort=//p' "$AMULE_CONFIG_DIR/amule.conf" | head -1)
	[ "$CONF_EC_PORT" = "$EC_PORT" ] ||
		_problem "AMULE_CONFIG_DIR has ECPort=$CONF_EC_PORT, but the suite uses EC_PORT=$EC_PORT"
fi

if [ -z "${AMULE_SHARED_DIR:-}" ]; then
	_problem "AMULE_SHARED_DIR is unset"
elif [ ! -d "$AMULE_SHARED_DIR" ] || [ ! -w "$AMULE_SHARED_DIR" ]; then
	_problem "AMULE_SHARED_DIR=$AMULE_SHARED_DIR is not a writable directory"
fi

if [ "${#PROBLEMS[@]}" -eq 0 ]; then
	# The daemon-side checks need a running amuleapi, which run-all.sh brings up.
	bash "$SCRIPT_DIR/run-all.sh" lib/live-preflight.sh || _problem "the live preflight failed (see above)"
fi

if [ "${#PROBLEMS[@]}" -gt 0 ]; then
	echo "Not starting the live run:" >&2
	printf '  - %s\n' "${PROBLEMS[@]}" >&2
	exit 2
fi

exec bash "$SCRIPT_DIR/run-all.sh" "$@"
