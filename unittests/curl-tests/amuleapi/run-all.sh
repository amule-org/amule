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

# Locate the repo root by climbing out of unittests/curl-tests/amuleapi/.
# AMULEAPI_ROOT remains an env override for unusual layouts; the default
# follows the script's own location so anyone who checks the repo out
# elsewhere works without editing this file.
ROOT="${AMULEAPI_ROOT:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
BIN="${AMULEAPI_BIN:-$ROOT/build-macos/src/webapi/amuleapi}"

if [ ! -x "$BIN" ]; then
	echo "FATAL: amuleapi binary not found at $BIN" >&2
	echo "       set AMULEAPI_BIN env var to point at it." >&2
	exit 2
fi

run_phase() {
	local script=$1
	echo "==================== $script ===================="
	# Narrowly target the regtest daemon so a dev who happens to have
	# `vim path/to/amuleapi.cpp` open doesn't get their editor killed.
	# The config-dir suffix is uniquely ours.
	pkill -f "amuleapi --config-dir=/tmp/amuleapi-regtest" 2>/dev/null
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
	# Poll /version until the daemon is ready instead of guessing the
	# cold-start time. The first EC GET_UPDATE roundtrip can take a
	# couple of seconds on a slow CI runner, and the cap of 12 leaves
	# headroom while still failing fast on a genuine bring-up bug.
	local i
	for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
		if curl -s -o /dev/null --max-time 1 \
		    http://localhost:4713/api/v0/version 2>/dev/null; then
			break
		fi
		sleep 0.5
	done
	# Phase scripts that need to bounce the daemon themselves
	# (phase9.sh rewrites amuleapi.conf to flip CORS modes) read
	# these envs to know how to restart cleanly.
	AMULEAPI_BIN="$BIN" \
	AMULEAPI_CONFIG_DIR=/tmp/amuleapi-regtest \
	AMULEAPI_LOG=/tmp/amuleapi.log \
	bash "$SCRIPT_DIR/$script"
	local rc=$?
	echo "$script exit=$rc"
	# If a phase failed AND the daemon is currently rate-limiting,
	# the operator likely hit the phase3 fallout: 7 deliberate
	# wrong-password attempts armed a 5-minute IP lockout that
	# subsequent phases inherit when the orchestrator's daemon
	# restart isn't enough to clear the in-memory bucket. Print
	# a one-line tip so the operator doesn't lose half an hour
	# chasing the wrong layer.
	if [ "$rc" -ne 0 ]; then
		local probe=$(curl -s -X POST -H "Content-Type: application/json" \
			-o /dev/null -w "%{http_code}" \
			-d "{\"password\":\"adminpass\"}" \
			http://localhost:4713/api/v0/auth/login 2>/dev/null)
		if [ "$probe" = "429" ]; then
			echo "TIP: amuleapi is currently rate-limiting login (HTTP 429)." \
			     "If you ran phase3 right before this, that's the 7-bad-pass" \
			     "arm carried over. Restart amuleapi (kills the bucket)" \
			     "before re-running."
		fi
	fi
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
	phase9.sh phase11.sh
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
