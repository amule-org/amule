#!/usr/bin/env bash
#
# amuleapi 25-cors — CORS opt-in.
#
# Wire contract:
#   * `AllowCORS=0` (default): no `Access-Control-*` headers on any
#     response. Browser cross-origin fetches blocked by the browser
#     per same-origin policy. `Vary: Origin` NOT set.
#   * `AllowCORS=1` + empty `CorsOriginAllowlist`: wildcard via echo.
#     Any request that carries an `Origin` header gets that origin
#     echoed in `Access-Control-Allow-Origin`. `Vary: Origin` is
#     always set so caches don't poison cross-origin responses.
#   * `AllowCORS=1` + non-empty `CorsOriginAllowlist`: an origin is
#     echoed only if it appears verbatim in the comma-separated
#     allowlist. Non-matching origins receive `Vary: Origin` but
#     no `Access-Control-Allow-Origin`.
#   * Credentials: when an origin is allowed,
#     `Access-Control-Allow-Credentials: true` is always set
#     (cookie-auth-compatible with the per-origin echo).
#   * Preflight: `OPTIONS` + `Access-Control-Request-Method` short-
#     circuits before auth, replies 204 with
#     `Access-Control-Allow-Methods: GET, HEAD, POST, PATCH, DELETE,
#     OPTIONS` and `Access-Control-Allow-Headers: Authorization,
#     Content-Type, If-None-Match, Last-Event-ID` and
#     `Access-Control-Max-Age: 86400`.
#   * SSE: the streaming response carries the same Allow-Origin /
#     Allow-Credentials / Vary headers.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
CONFIG_DIR=${AMULEAPI_CONFIG_DIR:-/tmp/amuleapi-regtest}
BIN=${AMULEAPI_BIN:-/Users/bitandyou/Sync/Utility/PlexBox/amule/amule-fiber/amule-src-amuleapi/build-macos/src/webapi/amuleapi}
LOG=${AMULEAPI_LOG:-/tmp/amuleapi.log}

FAIL_COUNT=0
TEST_COUNT=0

HDR=$(mktemp -t amuleapi_25_cors_hdr.XXXXXX)
BODY=$(mktemp -t amuleapi_25_cors_body.XXXXXX)
SSE=$(mktemp -t amuleapi_25_cors_sse.XXXXXX)
trap 'rm -f "$HDR" "$BODY" "$SSE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi
if [ ! -x "$BIN" ]; then
	_die "amuleapi binary not found at $BIN. Set AMULEAPI_BIN to override."
fi

echo "amuleapi 25-cors smoke @ $HOST (bin=$BIN config=$CONFIG_DIR)"

# Helper: rewrite amuleapi.conf with a given AllowCORS value and
# allowlist, then bounce the daemon. The first three sections are
# the defaults the orchestrator wrote; we only mutate [Server] so
# password files and the JWT secret stay valid across the restart.
_rewrite_cors_and_restart() {
	local allow=$1
	local allowlist=$2
	pkill -f "amuleapi --config-dir=$CONFIG_DIR" 2>/dev/null
	sleep 1
	cat > "$CONFIG_DIR/amuleapi.conf" <<EOF
[Server]
BindAddress=127.0.0.1
Port=4713
AllowCORS=$allow
CorsOriginAllowlist=$allowlist

[EC]
Host=127.0.0.1
Port=4712
Password=

[Auth]
LoginFailureWindowSeconds=60
LoginFailureThreshold=5
LoginLockoutSeconds=300
EOF
	"$BIN" --config-dir="$CONFIG_DIR" \
		--host=127.0.0.1 --port=4712 --password=amule \
		> "$LOG" 2>&1 &
	# Wait for /version to respond.
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
		if curl -s -o /dev/null --max-time 1 "$HOST/api/v0/version" 2>/dev/null; then
			return 0
		fi
		sleep 0.5
	done
	_die "daemon did not come back up after restart (allow=$allow allowlist=$allowlist)"
}

# Helper: capture status + headers + body of an arbitrary curl.
# Args: extra curl args. Result: HDR + BODY temp files populated.
_curl() {
	: > "$HDR"; : > "$BODY"
	curl -sS -o "$BODY" -D "$HDR" "$@" || true
}

# Helper: case-insensitive header lookup against HDR file.
# Returns the trimmed value of the first matching header.
_hdr() {
	local name=$1
	# Strip CRLF and leading "name:" with case-insensitive match.
	awk -v n="$name" '
		BEGIN { IGNORECASE = 1 }
		match($0, "^"n"[: ]+") {
			v = substr($0, RSTART + RLENGTH);
			sub(/\r$/, "", v);
			sub(/^[[:space:]]+/, "", v);
			print v;
			exit
		}
	' "$HDR"
}

# --- Mode A: AllowCORS=0 (default). ------------------------------
_rewrite_cors_and_restart 0 ""

_curl -H "Origin: https://app.example.com" "$HOST/api/v0/version"
ACAO=$(_hdr "Access-Control-Allow-Origin")
VARY=$(_hdr "Vary")
if [ -z "$ACAO" ]; then
	_pass "AllowCORS=0: no Access-Control-Allow-Origin on response"
else
	_fail "AllowCORS=0 leaks Access-Control-Allow-Origin" "got: $ACAO"
fi
if [ -z "$VARY" ] || ! echo "$VARY" | grep -qi "Origin"; then
	_pass "AllowCORS=0: no Vary: Origin on response"
else
	_fail "AllowCORS=0 leaks Vary: Origin" "got: $VARY"
fi

# --- Mode B: AllowCORS=1 with empty allowlist (wildcard). --------
_rewrite_cors_and_restart 1 ""

_curl -H "Origin: https://wild.example.com" "$HOST/api/v0/version"
ACAO=$(_hdr "Access-Control-Allow-Origin")
ACAC=$(_hdr "Access-Control-Allow-Credentials")
ACEH=$(_hdr "Access-Control-Expose-Headers")
VARY=$(_hdr "Vary")
if [ "$ACAO" = "https://wild.example.com" ]; then
	_pass "AllowCORS=1 (wildcard): Access-Control-Allow-Origin echoes the Origin verbatim"
else
	_fail "Wildcard echo" "expected 'https://wild.example.com', got '$ACAO'"
fi
if [ "$ACAC" = "true" ]; then
	_pass "AllowCORS=1: Access-Control-Allow-Credentials: true"
else
	_fail "Allow-Credentials" "expected 'true', got '$ACAC'"
fi
if echo "$ACEH" | grep -qi "ETag"; then
	_pass "AllowCORS=1: Access-Control-Expose-Headers lists ETag"
else
	_fail "Expose-Headers" "expected to contain ETag, got '$ACEH'"
fi
if echo "$VARY" | grep -qi "Origin"; then
	_pass "AllowCORS=1: Vary: Origin set"
else
	_fail "Vary: Origin" "expected 'Origin', got '$VARY'"
fi

# Same daemon, but request without an Origin header. The server
# should NOT add Access-Control-Allow-Origin (no origin to echo)
# but SHOULD still set Vary: Origin (CORS is on).
_curl "$HOST/api/v0/version"
ACAO=$(_hdr "Access-Control-Allow-Origin")
VARY=$(_hdr "Vary")
if [ -z "$ACAO" ]; then
	_pass "AllowCORS=1: request without Origin → no Allow-Origin"
else
	_fail "Spurious Allow-Origin" "no Origin sent, but got: $ACAO"
fi
if echo "$VARY" | grep -qi "Origin"; then
	_pass "AllowCORS=1: Vary: Origin set even when request has no Origin"
else
	_fail "Vary: Origin when CORS on" "got: $VARY"
fi

# --- Mode C: AllowCORS=1 with a per-origin allowlist. ------------
_rewrite_cors_and_restart 1 "https://allowed.example.com,https://also.example.com"

_curl -H "Origin: https://allowed.example.com" "$HOST/api/v0/version"
ACAO=$(_hdr "Access-Control-Allow-Origin")
if [ "$ACAO" = "https://allowed.example.com" ]; then
	_pass "Allowlist: matching Origin echoes back"
else
	_fail "Allowlist match" "expected echo, got '$ACAO'"
fi

_curl -H "Origin: https://also.example.com" "$HOST/api/v0/version"
ACAO=$(_hdr "Access-Control-Allow-Origin")
if [ "$ACAO" = "https://also.example.com" ]; then
	_pass "Allowlist: second entry matches and echoes"
else
	_fail "Allowlist second entry" "expected echo, got '$ACAO'"
fi

_curl -H "Origin: https://attacker.example.com" "$HOST/api/v0/version"
ACAO=$(_hdr "Access-Control-Allow-Origin")
VARY=$(_hdr "Vary")
if [ -z "$ACAO" ]; then
	_pass "Allowlist: non-matching Origin → no Allow-Origin"
else
	_fail "Allowlist rejects" "expected no Allow-Origin, got '$ACAO'"
fi
if echo "$VARY" | grep -qi "Origin"; then
	_pass "Allowlist: Vary: Origin set even on rejected origin"
else
	_fail "Vary on rejection" "got: $VARY"
fi

# --- OPTIONS preflight (Mode C, allowed origin). -----------------
_curl -X OPTIONS \
	-H "Origin: https://allowed.example.com" \
	-H "Access-Control-Request-Method: POST" \
	-H "Access-Control-Request-Headers: Authorization, Content-Type" \
	"$HOST/api/v0/downloads"
STATUS=$(head -1 "$HDR" | awk '{print $2}')
ACAO=$(_hdr "Access-Control-Allow-Origin")
ACAM=$(_hdr "Access-Control-Allow-Methods")
ACAH=$(_hdr "Access-Control-Allow-Headers")
ACMA=$(_hdr "Access-Control-Max-Age")

if [ "$STATUS" = "204" ]; then
	_pass "Preflight OPTIONS → 204"
else
	_fail "Preflight status" "expected 204, got '$STATUS'"
fi
if [ "$ACAO" = "https://allowed.example.com" ]; then
	_pass "Preflight: Allow-Origin echoes allowed origin"
else
	_fail "Preflight Allow-Origin" "expected echo, got '$ACAO'"
fi
if echo "$ACAM" | grep -q "POST" && echo "$ACAM" | grep -q "PATCH" \
   && echo "$ACAM" | grep -q "DELETE"; then
	_pass "Preflight: Allow-Methods lists mutating verbs"
else
	_fail "Allow-Methods" "expected POST/PATCH/DELETE listed, got '$ACAM'"
fi
if echo "$ACAH" | grep -qi "Authorization" \
   && echo "$ACAH" | grep -qi "If-None-Match" \
   && echo "$ACAH" | grep -qi "Last-Event-ID"; then
	_pass "Preflight: Allow-Headers lists Authorization, If-None-Match, Last-Event-ID"
else
	_fail "Allow-Headers" "missing one of the expected headers, got '$ACAH'"
fi
if [ "$ACMA" = "86400" ]; then
	_pass "Preflight: Max-Age == 86400 (24h preflight cache)"
else
	_fail "Max-Age" "expected 86400, got '$ACMA'"
fi

# Preflight from a non-allowed origin: 204 with no Allow-Origin.
# Browser will then block the actual request.
_curl -X OPTIONS \
	-H "Origin: https://attacker.example.com" \
	-H "Access-Control-Request-Method: POST" \
	"$HOST/api/v0/downloads"
STATUS=$(head -1 "$HDR" | awk '{print $2}')
ACAO=$(_hdr "Access-Control-Allow-Origin")
if [ "$STATUS" = "204" ] && [ -z "$ACAO" ]; then
	_pass "Preflight (non-allowed origin) → 204 with no Allow-Origin"
else
	_fail "Preflight rejection" \
		"expected 204 + no Allow-Origin, got status='$STATUS' Allow-Origin='$ACAO'"
fi

# --- SSE response CORS headers (mode C, allowed origin). ---------
#
# Mode C is still active. Capture only the response head — we don't
# need the stream body for header verification. Curl handles SSE
# the same as any HTTP/1.1 chunked response; -I won't work because
# HEAD doesn't trigger the streaming handler, so we do a short -m 2.
: > "$HDR"; : > "$SSE"
ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"
curl -sS -m 2 -D "$HDR" -o "$SSE" \
	-H "Origin: https://allowed.example.com" \
	-H "Authorization: Bearer $ADMIN_TOKEN" \
	"$HOST/api/v0/events" >/dev/null 2>&1 || true
ACAO=$(_hdr "Access-Control-Allow-Origin")
ACAC=$(_hdr "Access-Control-Allow-Credentials")
CT=$(_hdr "Content-Type")
if [ "$ACAO" = "https://allowed.example.com" ]; then
	_pass "SSE response: Allow-Origin set for allowed origin"
else
	_fail "SSE Allow-Origin" "expected echo, got '$ACAO'"
fi
if [ "$ACAC" = "true" ]; then
	_pass "SSE response: Allow-Credentials: true"
else
	_fail "SSE Allow-Credentials" "expected 'true', got '$ACAC'"
fi
if echo "$CT" | grep -qi "text/event-stream"; then
	_pass "SSE response: Content-Type unchanged by CORS path"
else
	_fail "SSE Content-Type" "expected text/event-stream, got '$CT'"
fi

# --- Cleanup: restore AllowCORS=0 so subsequent manual smokes don't
#     inherit phase 9's CORS-enabled config when re-run in the same
#     /tmp/amuleapi-regtest. run-all.sh wipes between phases anyway,
#     but this protects standalone invocations.
_rewrite_cors_and_restart 0 ""

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
