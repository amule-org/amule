#!/usr/bin/env bash
#
# Orchestrator for the amuleapi curl-smoke matrix.
#
# Brings up a fresh amuleapi daemon for each script, runs the
# script, and aggregates pass/fail. The fresh-daemon-per-script
# pattern isolates state — most importantly, 02-auth.sh fires the
# rate-limit lockout (5 failed logins → 300 s IP lockout) which would
# block every subsequent script's /auth/login. Restarting wipes the
# in-memory CRateLimiter buckets.
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
#   ./run-all.sh                                                 # run every script in
#                                                                # the canonical order
#   ./run-all.sh 12-downloads-add-patch.sh 13-downloads-delete-clear.sh  # run a subset

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

# One stable 256-bit JWT secret (64 hex chars) reused by every phase's
# daemon. See run_phase: a fixed secret makes a brief instance overlap
# on the port harmless instead of a token-verification failure.
JWT_SECRET=$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')

run_phase() {
	local script=$1
	echo "==================== $script ===================="
	# Narrowly target the regtest daemon so a dev who happens to have
	# `vim path/to/amuleapi.cpp` open doesn't get their editor killed.
	# The config-dir suffix is uniquely ours.
	#
	# Tear down the previous phase's daemon CLEANLY before reusing port
	# 4713. A bare `pkill; sleep 1` races graceful shutdown: amuleapi
	# binds with SO_REUSEADDR, so a lingering old instance can briefly
	# share the port with the new one. Each phase regenerates its config
	# dir (and its JWT secret), so during that overlap a token minted by
	# one instance is rejected by the other ("invalid or expired token").
	# Wait for every matching process to exit, escalating to SIGKILL, and
	# for the listen socket to be released.
	local pat="amuleapi --config-dir=/tmp/amuleapi-regtest"
	pkill -f "$pat" 2>/dev/null
	local w
	for w in $(seq 1 40); do
		pgrep -f "$pat" >/dev/null 2>&1 || break
		sleep 0.25
	done
	if pgrep -f "$pat" >/dev/null 2>&1; then
		pkill -9 -f "$pat" 2>/dev/null
		for w in $(seq 1 20); do
			pgrep -f "$pat" >/dev/null 2>&1 || break
			sleep 0.25
		done
	fi
	for w in $(seq 1 40); do
		ss -tln 2>/dev/null | grep -q ":4713 " || break
		sleep 0.25
	done
	rm -rf /tmp/amuleapi-regtest
	mkdir -p /tmp/amuleapi-regtest
	# Pin a stable JWT secret for every phase (belt-and-suspenders with
	# the clean teardown above): if two instances ever do overlap on the
	# port, they share the secret, so a freshly-minted token still
	# verifies regardless of which instance answers. amuleapi loads this
	# file as-is instead of generating a per-process secret.
	printf '%s' "$JWT_SECRET" > /tmp/amuleapi-regtest/amuleapi-jwt-secret
	chmod 600 /tmp/amuleapi-regtest/amuleapi-jwt-secret
	"$BIN" --config-dir=/tmp/amuleapi-regtest \
		--host=127.0.0.1 --port=4712 --password=amule \
		--set-admin-pass=adminpass > /dev/null 2>&1
	"$BIN" --config-dir=/tmp/amuleapi-regtest \
		--host=127.0.0.1 --port=4712 --password=amule \
		--set-guest-pass=guestpass > /dev/null 2>&1
	# 27-static-frontend exercises the static-frontend serve path. It
	# plants symlinks + an oversized file into StaticRoot during the
	# run, so the dir has to be writable. The bundled source tree is
	# read-only in container CI; copy the placeholder out to a /tmp
	# scratch dir and point StaticRoot at the copy. Other scripts
	# leave it empty so the install-path discovery chain stays
	# exercised by 27-static-frontend only.
	if [ "$script" = "27-static-frontend.sh" ]; then
		STATIC_SRC="$ROOT/src/webapi/static"
		STATIC_DIR=/tmp/amuleapi-static-frontend
		rm -rf "$STATIC_DIR"
		mkdir -p "$STATIC_DIR"
		if [ -d "$STATIC_SRC" ]; then
			cp -R "$STATIC_SRC"/. "$STATIC_DIR/"
		fi
		sed -i'.bak' \
			"s|^StaticRoot=.*|StaticRoot=$STATIC_DIR|" \
			/tmp/amuleapi-regtest/amuleapi.conf
		rm -f /tmp/amuleapi-regtest/amuleapi.conf.bak
	fi
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
	# Scripts that bounce the daemon themselves (25-cors.sh rewrites
	# amuleapi.conf to flip CORS modes) read these envs to know how
	# to restart cleanly. AMULE_SHARED_DIR lets a phase self-provision
	# a shared-file fixture (17-shared-priority-patch): it points at a
	# directory the connected amuled shares (its Incoming, typically),
	# so the smoke doesn't depend on the operator's library already
	# holding a shared file. Unset is fine unless a phase needs it.
	AMULEAPI_BIN="$BIN" \
	AMULEAPI_CONFIG_DIR=/tmp/amuleapi-regtest \
	AMULEAPI_LOG=/tmp/amuleapi.log \
	AMULE_SHARED_DIR="${AMULE_SHARED_DIR:-}" \
	bash "$SCRIPT_DIR/$script"
	local rc=$?
	echo "$script exit=$rc"
	# If a script failed AND the daemon is currently rate-limiting,
	# the operator likely hit the 02-auth fallout: 7 deliberate
	# wrong-password attempts armed a 5-minute IP lockout that later
	# scripts inherit when the orchestrator's daemon restart isn't
	# enough to clear the in-memory bucket. Print a one-line tip so
	# the operator doesn't lose half an hour chasing the wrong layer.
	if [ "$rc" -ne 0 ]; then
		local probe=$(curl -s -X POST -H "Content-Type: application/json" \
			-o /dev/null -w "%{http_code}" \
			-d "{\"password\":\"adminpass\"}" \
			http://localhost:4713/api/v0/auth/login 2>/dev/null)
		if [ "$probe" = "429" ]; then
			echo "TIP: amuleapi is currently rate-limiting login (HTTP 429)." \
			     "If you ran 02-auth.sh right before this, that's the 7-bad-pass" \
			     "arm carried over. Restart amuleapi (kills the bucket)" \
			     "before re-running."
		fi
	fi
	return $rc
}

# Canonical execution order. Numeric prefix doubles as dependency
# ordering: auth before any mutation, refresher-consolidation tests
# before later read tests that rely on the consolidated tick shape,
# CORS / static-frontend after the API surface tests so failures in
# the new transports don't mask earlier regressions.
PHASES=(
	01-version-and-errors.sh
	02-auth.sh
	03-read-status.sh
	04-read-downloads-shared.sh
	05-read-servers-kad-categories-prefs.sh
	06-read-logs.sh
	07-read-stats-and-search-results.sh
	08-read-download-parts.sh
	09-refresher-consolidation.sh
	10-refresher-lazy-ondemand.sh
	11-downloads-default-filter.sh
	12-downloads-add-patch.sh
	13-downloads-delete-clear.sh
	14-servers-mutations.sh
	15-preferences-patch.sh
	16-networks-connect.sh
	17-shared-priority-patch.sh
	18-categories-crud.sh
	19-search.sh
	20-etag-conditional-get.sh
	21-sse-heartbeat.sh
	22-sse-diff-emission.sh
	23-sse-replay.sh
	24-sse-resync.sh
	25-cors.sh
	26-rfc-followup-endpoints.sh
	27-static-frontend.sh
	28-list-pagination-sort.sh
	29-bulk-mutations.sh
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

# Final teardown — same narrow scope as the per-phase kill at line 53
# so an editor with `vim path/to/amuleapi.cpp` open survives the run.
pkill -f "amuleapi --config-dir=/tmp/amuleapi-regtest" 2>/dev/null
echo
if [ "$OVERALL" -eq 0 ]; then
	echo "OVERALL: ALL PHASES PASSED"
else
	echo "OVERALL: ONE OR MORE PHASES FAILED"
fi
exit "$OVERALL"
