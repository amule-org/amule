#!/usr/bin/env bash
#
# Orchestrator for the amuleapi curl-smoke matrix.
#
# Brings up a fresh amuleapi daemon for each phase script, runs the
# script, and aggregates pass/fail. The fresh-daemon-per-phase pattern
# isolates state across scripts — most importantly, phase3.sh fires
# the rate-limit lockout (5 failed logins → 300 s IP lockout) which
# would block every subsequent phase's /auth/login. Restarting wipes
# the in-memory CRateLimiter buckets.
#
# Setup per phase:
#   1. pkill any running amuleapi
#   2. wipe + recreate /tmp/amuleapi-regtest config dir
#   3. set admin pass (one-shot CLI invocation, daemon exits)
#   4. set guest pass (second one-shot — App.cpp's set-pass paths are
#      mutually exclusive, so admin and guest need separate runs)
#   5. start the daemon in foreground, log to /tmp/amuleapi.log
#   6. sleep 5 s for the refresher to populate its caches (first
#      GET_UPDATE tick is the heaviest — sends every alive ECID with
#      full identity)
#   7. run the phase script
#
# Usage:
#   ./run-all.sh                         # run every phase script in
#                                        # alphanumeric order
#   ./run-all.sh phase5a.sh phase5b.sh   # run a subset

set -u

# Resolve our location so the orchestrator runs from any cwd.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

# Default: the amuleapi worktree this script ships with.
ROOT="${AMULEAPI_ROOT:-/Users/bitandyou/Sync/Utility/PlexBox/amule/amule-fiber/amule-src-amuleapi}"
BIN="${AMULEAPI_BIN:-$ROOT/build-macos/src/webapi/amuleapi}"

if [ ! -x "$BIN" ]; then
	echo "FATAL: amuleapi binary not found at $BIN" >&2
	echo "       set AMULEAPI_BIN env var to point at it." >&2
	exit 2
fi

run_phase() {
	local script=$1
	echo "==================== $script ===================="
	pkill -f amuleapi 2>/dev/null
	sleep 1
	rm -rf /tmp/amuleapi-regtest
	mkdir -p /tmp/amuleapi-regtest
	"$BIN" --config-dir=/tmp/amuleapi-regtest \
		--host=127.0.0.1 --port=4712 --password=amule \
		--set-admin-pass=adminpass > /dev/null 2>&1
	"$BIN" --config-dir=/tmp/amuleapi-regtest \
		--host=127.0.0.1 --port=4712 --password=amule \
		--set-guest-pass=guestpass > /dev/null 2>&1
	"$BIN" --config-dir=/tmp/amuleapi-regtest \
		--host=127.0.0.1 --port=4712 --password=amule \
		> /tmp/amuleapi.log 2>&1 &
	sleep 1
	# Phase scripts that need to bounce the daemon themselves
	# (phase9.sh rewrites amuleapi.conf to flip CORS modes) read
	# these envs to know how to restart cleanly.
	AMULEAPI_BIN="$BIN" \
	AMULEAPI_CONFIG_DIR=/tmp/amuleapi-regtest \
	AMULEAPI_LOG=/tmp/amuleapi.log \
	bash "$SCRIPT_DIR/$script"
	local rc=$?
	echo "$script exit=$rc"
	return $rc
}

# Phase list: legacy phases first (in canonical order), then Phase 5
# sub-phases. Only scripts that exist are run — sub-phases not yet
# landed are skipped.
PHASES=(
	phase2.sh phase3.sh phase4.sh phase4b.sh phase4c.sh phase4c-bis.sh
	phase4d.sh phase4e.sh phase4f.sh phase4g.sh phase4h.sh
	phase5a.sh phase5b.sh phase5c.sh phase5d.sh phase5e.sh
	phase5f.sh phase5g.sh phase6.sh phase7.sh
	phase8a.sh phase8b.sh phase8c.sh phase8d.sh
	phase9.sh
)

# Override list from the command line if given.
if [ "$#" -gt 0 ]; then
	PHASES=("$@")
fi

OVERALL=0
for s in "${PHASES[@]}"; do
	if [ ! -f "$SCRIPT_DIR/$s" ]; then
		echo "skip $s (not present in $SCRIPT_DIR)"
		continue
	fi
	if ! run_phase "$s"; then
		OVERALL=1
	fi
done

pkill -f amuleapi 2>/dev/null
echo
if [ "$OVERALL" -eq 0 ]; then
	echo "OVERALL: ALL PHASES PASSED"
else
	echo "OVERALL: ONE OR MORE PHASES FAILED"
fi
exit "$OVERALL"
