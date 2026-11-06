#!/usr/bin/env bash
#
# amuleapi 50-proxy-auth -- sign-in through a reverse proxy's user header.
#
# The phase rewrites [Auth] in amuleapi.conf and restarts the daemon for
# each mode. Wire contract:
#   * With ProxyAuthHeader empty the header means nothing.
#   * GET /auth/session from a trusted address with the header and no valid
#     session starts one: Set-Cookie, `user` set, `logout_url` from config.
#   * The header alone authenticates nothing else.
#   * A proxy session needs the same header from a trusted address on every
#     request, so a missing or different user, an untrusted address or the
#     option turned off gives 401.
#   * A header sent twice, in any spelling, is no header.
#   * A changed ProxyAuthRole ends older proxy sessions.
#   * A password change does not end a proxy session. Password logins keep
#     working, and their sessions have `user: null`.
#   * ProxyAuthHeader without ProxyAuthTrustedAddresses stops the daemon.

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

CONFIG_DIR=${AMULEAPI_CONFIG_DIR:-/tmp/amuleapi-regtest}
ROOT=${AMULEAPI_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
BIN=${AMULEAPI_BIN:-$(_find_amuleapi_bin "$ROOT")}
LOG=${AMULEAPI_LOG:-/tmp/amuleapi.log}
EC_HOST=${EC_HOST:-127.0.0.1}
EC_PORT=${EC_PORT:-4712}
CONF="$CONFIG_DIR/amuleapi.conf"
# By address: "localhost" may be tried as ::1 first, and the trusted list
# is matched against the address the daemon sees.
P="http://127.0.0.1:${HOST##*:}/api/v1"

ORIG_CONF=$(mktemp -t amuleapi_50_conf.XXXXXX)
_on_exit 'rm -f "$ORIG_CONF"'

_preflight
[ -x "$BIN" ] || _die "no usable amuleapi binary under $ROOT. Set AMULEAPI_BIN to point at one."
grep -q '^ProxyAuthHeader=' "$CONF" || _die "$CONF has no ProxyAuthHeader key; is $BIN current?"
cp "$CONF" "$ORIG_CONF"

echo "amuleapi 50-proxy-auth smoke @ $P (bin=$BIN config=$CONFIG_DIR)"

_stop() {
	pkill -f "amuleapi --config-dir=$CONFIG_DIR" 2>/dev/null
	local _w
	for _w in $(seq 1 40); do
		pgrep -f "amuleapi --config-dir=$CONFIG_DIR" >/dev/null 2>&1 || return 0
		sleep 0.25
	done
	pkill -9 -f "amuleapi --config-dir=$CONFIG_DIR" 2>/dev/null
	sleep 0.5
}

# Args: header, trusted addresses, role, logout URL.
_write_conf() {
	cp "$ORIG_CONF" "$CONF"
	sed -i'.bak' \
		-e "s|^ProxyAuthHeader=.*|ProxyAuthHeader=$1|" \
		-e "s|^ProxyAuthTrustedAddresses=.*|ProxyAuthTrustedAddresses=$2|" \
		-e "s|^ProxyAuthRole=.*|ProxyAuthRole=$3|" \
		-e "s|^ProxyAuthLogoutUrl=.*|ProxyAuthLogoutUrl=$4|" \
		"$CONF"
	rm -f "$CONF.bak"
	chmod 600 "$CONF"
}

_start() {
	"$BIN" --config-dir="$CONFIG_DIR" --host="$EC_HOST" --port="$EC_PORT" > "$LOG" 2>&1 &
	local _i
	for _i in $(seq 1 30); do
		curl -s -o /dev/null --max-time 1 "$P/health" 2>/dev/null && return 0
		sleep 0.5
	done
	_die "daemon did not come back up (conf: $(grep '^ProxyAuth' "$CONF" | tr '\n' ' '))"
}

_restart() {
	_stop
	_write_conf "$@"
	_start
}

# Session cookie value from the last response, or nothing.
_cookie() { _header set-cookie | sed -n 's/^amuleapi_token=\([^;]*\).*/\1/p'; }

LOGOUT_URL="https://sso.example/logout"

# --- Off: the header means nothing. ----------------------------------
echo "--- ProxyAuthHeader empty ---"
_curl -H "Remote-User: alice" "$P/auth/session"
_assert_status 401 "GET /auth/session with Remote-User, option off"

# --- On, trusted address, admin role. --------------------------------
echo "--- trusted proxy, admin role ---"
_restart Remote-User "127.0.0.1,::1" admin "$LOGOUT_URL"
if grep -q "reverse proxy sign-in is on" "$LOG"; then
	_pass "startup log warns that proxy sign-in is on"
else
	_fail "startup log warns that proxy sign-in is on" "log: $(head -c 600 "$LOG")"
fi

_curl -H "Remote-User: alice" "$P/auth/session"
_assert_status 200 "GET /auth/session with Remote-User starts a session"
_assert_json_eq '.user' alice "session user is the header value"
_assert_json_eq '.role' admin "session role is ProxyAuthRole"
_assert_json_eq '.logout_url' "$LOGOUT_URL" "session carries ProxyAuthLogoutUrl"
_assert_json_eq 'has("token")' false "the token is not in the body"
_assert_header_contains "HttpOnly; SameSite=Strict" "session cookie is HttpOnly, SameSite=Strict"
_assert_header_contains "Cache-Control: private, no-store" "session is private, no-store"
ALICE=$(_cookie)
ALICE_SID=$(_jq '.session_id')
[ -n "$ALICE" ] || _die "no session cookie from the proxy sign-in"

_curl -H "Cookie: amuleapi_token=$ALICE" -H "Remote-User: alice" "$P/auth/session"
_assert_status 200 "GET /auth/session with the cookie and the same user"
_assert_json_eq '.session_id' "$ALICE_SID" "the existing session is kept"
_assert_eq "" "$(_cookie)" "no new cookie for a session that is still valid"

_curl -H "Cookie: amuleapi_token=$ALICE" -H "Remote-User: alice" "$P/auth/passwords"
_assert_status 200 "admin route with the proxy cookie and header"

_curl -H "Remote-User: alice" "$P/auth/passwords"
_assert_status 401 "the header alone does not authenticate a request"

_curl -H "Cookie: amuleapi_token=$ALICE" "$P/auth/passwords"
_assert_status 401 "proxy cookie without the header"

_curl -H "Cookie: amuleapi_token=$ALICE" -H "Remote-User: bob" "$P/auth/passwords"
_assert_status 401 "proxy cookie with another user"

_curl -H "Cookie: amuleapi_token=$ALICE" -H "Remote-User: alice" -H "remote-user: alice" "$P/auth/passwords"
_assert_status 401 "header sent twice"

# A proxy that appends leaves the client's copy first.
_curl -H "Remote-User: mallory" -H "Remote-User: alice" "$P/auth/session"
_assert_status 401 "two copies of the header in the same spelling start no session"

_curl -H "Cookie: amuleapi_token=$ALICE" -H "Remote-User: bob" "$P/auth/session"
_assert_status 200 "GET /auth/session as another user replaces the session"
_assert_json_eq '.user' bob "new session is for the new user"
BOB=$(_cookie)
[ -n "$BOB" ] && [ "$BOB" != "$ALICE" ] && _pass "a new cookie is set" || _fail "a new cookie is set" "cookie: $BOB"

# --- Password sessions are unchanged. --------------------------------
TOKEN=$(_login "$ADMIN_PASS" "$P")
[ -n "$TOKEN" ] && _pass "password login still works" || _fail "password login still works"
_curl -H "Authorization: Bearer $TOKEN" "$P/auth/session"
_assert_status 200 "GET /auth/session with a password token"
_assert_json_eq '.user' null "password session has user null"
_assert_json_eq '.logout_url' null "password session has logout_url null"
_curl -H "Authorization: Bearer $TOKEN" "$P/auth/passwords"
_assert_status 200 "password token needs no proxy header"

# A credential change ends password sessions, not proxy ones. The cutoff is
# whole seconds, so let the proxy session's iat fall behind it.
sleep 1.1
_curl -X PATCH -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d "{\"current_password\":\"$ADMIN_PASS\",\"admin_password\":\"$ADMIN_PASS\"}" "$P/auth/passwords"
_assert_status 200 "PATCH /auth/passwords"
_curl -H "Authorization: Bearer $TOKEN" "$P/auth/passwords"
_assert_status 401 "old password token is ended by the change"
_curl -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/passwords"
_assert_status 200 "proxy session survives a password change"

# --- Logout. ---------------------------------------------------------
_curl -X POST -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/logout"
_assert_status 204 "logout of a proxy session"
_curl -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/passwords"
_assert_status 401 "revoked proxy session is refused"
_curl -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/session"
_assert_status 200 "GET /auth/session after logout signs the user back in"
_assert_json_eq '.user' bob "the new session is for the same user"
BOB=$(_cookie)

# --- Guest role. -----------------------------------------------------
echo "--- trusted proxy, guest role, no logout URL ---"
_restart Remote-User "127.0.0.1" guest ""
_curl -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/passwords"
_assert_status 401 "admin proxy session ends when ProxyAuthRole becomes guest"
_curl -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/session"
_assert_status 200 "GET /auth/session after the role change"
_assert_json_eq '.role' guest "the new session has the new role"
_assert_json_eq '.user' bob "the new session is for the same user"
_curl -H "Remote-User: carol" "$P/auth/session"
_assert_status 200 "GET /auth/session as guest"
_assert_json_eq '.role' guest "session role is guest"
_assert_json_eq '.logout_url' null "logout_url is null when not configured"
CAROL=$(_cookie)
_curl -H "Cookie: amuleapi_token=$CAROL" -H "Remote-User: carol" "$P/auth/passwords"
_assert_status 403 "guest proxy session cannot use an admin route"

# --- Untrusted address. ----------------------------------------------
echo "--- proxy trusted only on ::1 ---"
_restart Remote-User "::1" admin ""
_curl -H "Remote-User: alice" "$P/auth/session"
_assert_status 401 "header from an untrusted address starts no session"
_curl -H "Remote-User: alice" -H "X-Forwarded-For: ::1" "$P/auth/session"
_assert_status 401 "X-Forwarded-For does not make an address trusted"
_curl -H "Cookie: amuleapi_token=$CAROL" -H "Remote-User: carol" "$P/auth/session"
_assert_status 401 "proxy session from an untrusted address"

# --- Option turned off after sessions exist. -------------------------
echo "--- ProxyAuthHeader turned off ---"
_restart "" "" admin ""
_curl -H "Cookie: amuleapi_token=$BOB" -H "Remote-User: bob" "$P/auth/session"
_assert_status 401 "proxy session ends when the option is off"
TOKEN=$(_login "$ADMIN_PASS" "$P")
_curl -H "Authorization: Bearer $TOKEN" "$P/auth/session"
_assert_status 200 "password session still works with the option off"

# --- Refuses to start without a trusted address. ---------------------
echo "--- ProxyAuthHeader without ProxyAuthTrustedAddresses ---"
_stop
_write_conf Remote-User "" admin ""
"$BIN" --config-dir="$CONFIG_DIR" --host="$EC_HOST" --port="$EC_PORT" > "$LOG" 2>&1 &
BAD_PID=$!
for _ in $(seq 1 40); do
	kill -0 "$BAD_PID" 2>/dev/null || break
	sleep 0.25
done
if kill -0 "$BAD_PID" 2>/dev/null; then
	kill "$BAD_PID" 2>/dev/null
	_fail "daemon refuses to start" "still running after 10 s"
else
	_pass "daemon refuses to start"
fi
if grep -q "ProxyAuthTrustedAddresses" "$LOG"; then
	_pass "the error names ProxyAuthTrustedAddresses"
else
	_fail "the error names ProxyAuthTrustedAddresses" "log: $(head -c 600 "$LOG")"
fi

# Leave the daemon as the orchestrator started it.
cp "$ORIG_CONF" "$CONF"
chmod 600 "$CONF"
_start

_summary
