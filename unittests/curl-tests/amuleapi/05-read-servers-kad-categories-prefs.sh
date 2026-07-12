#!/usr/bin/env bash
#
# amuleapi 05-read-servers-kad-categories-prefs — /servers, /kad, /categories, /preferences.
# Exercises the four endpoints added in this sub-phase. Tolerant of
# empty caches (e.g. amuled with no configured servers) — asserts
# envelope shape + per-item field types without requiring specific
# content.
#
# Bring-up:
#   amuleapi --config-dir=/tmp/amuleapi-05-read-servers-kad-categories-prefs \
#            --host=127.0.0.1 --port=4712 --password=amule \
#            --set-admin-pass=adminpass
#   amuleapi --config-dir=/tmp/amuleapi-05-read-servers-kad-categories-prefs ... &
#   ./05-read-servers-kad-categories-prefs.sh

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_05_read_servers_kad_categories_prefs_body.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	local resp
	resp=$(curl -s --max-time 10 -o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS" \
			"body head: $(printf '%s' "$CURL_BODY" | head -c 200)"
	fi
}

_assert_json_eq() {
	local expr=$1 expected=$2 label=$3
	local actual
	actual=$(printf '%s' "$CURL_BODY" | jq -r "$expr" 2>/dev/null) \
		|| _fail "$label" "body was not valid JSON" "body: $CURL_BODY"
	if [ "$actual" = "$expected" ]; then
		_pass "$label"
	else
		_fail "$label" "expected $expected, got $actual" "body: $CURL_BODY"
	fi
}

if ! command -v jq >/dev/null 2>&1; then
	_die "jq is required."
fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable."
fi

echo "amuleapi 05-read-servers-kad-categories-prefs smoke @ $HOST"

# Log in.
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# Wait for the first full refresher tick (servers + prefs land at the
# end of the tick — we need the second tick to confirm steady-state).
sleep 3

# --- 1. /servers ---------------------------------------------------
_curl "$HOST/api/v0/servers"
_assert_status 401 "GET /servers (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/servers"
_assert_status 200 "GET /servers (admin) → 200"
_assert_json_eq '.servers | type'          array  '/servers .servers is an array'
COUNT=$(printf '%s' "$CURL_BODY" | jq '.servers | length')
if [ "$COUNT" -gt 0 ]; then
	echo "  --- /servers has $COUNT entry/entries; per-item shape ---"
	_assert_json_eq '.servers[0].name     | type' string  '/servers[0].name is string'
	_assert_json_eq '.servers[0].address  | type' string  '/servers[0].address is string'
	_assert_json_eq '.servers[0].port     | type' number  '/servers[0].port is numeric'
	_assert_json_eq '.servers[0].users    | type' number  '/servers[0].users is numeric'
	_assert_json_eq '.servers[0].priority | test("^(low|normal|high)$")' \
		true '/servers[0].priority is a known enum value'
	_assert_json_eq '.servers[0].static   | type' boolean '/servers[0].static is boolean'
	# #440 server country: always-present ISO 3166-1 alpha-2 string,
	# empty when GeoIP is off/unresolved (never absent/null).
	_assert_json_eq '.servers[0].country_code | type' string '/servers[0].country_code is string (#440)'
fi

# --- 2. /kad -------------------------------------------------------
_curl "$HOST/api/v0/kad"
_assert_status 401 "GET /kad (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/kad"
_assert_status 200 "GET /kad (admin) → 200"
_assert_json_eq '.state | test("^(disabled|connecting|connected)$")' \
	true '/kad.state is a known enum value'
_assert_json_eq '.firewalled       | type' boolean '/kad.firewalled is boolean'
_assert_json_eq '.firewalled_udp   | type' boolean '/kad.firewalled_udp is boolean'
_assert_json_eq '.in_lan_mode      | type' boolean '/kad.in_lan_mode is boolean'
_assert_json_eq '.ip               | type' string  '/kad.ip is string'
_assert_json_eq '.network.users    | type' number  '/kad.network.users is numeric'
_assert_json_eq '.network.files    | type' number  '/kad.network.files is numeric'
_assert_json_eq '.network.nodes    | type' number  '/kad.network.nodes is numeric'
_assert_json_eq '.indexed.sources  | type' number  '/kad.indexed.sources is numeric'
_assert_json_eq '.buddy.status     | test("^(no_buddy|connecting|connected|unknown)$")' \
	true '/kad.buddy.status is a known enum value'

# --- 3. /categories -----------------------------------------------
_curl "$HOST/api/v0/categories"
_assert_status 401 "GET /categories (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/categories"
_assert_status 200 "GET /categories (admin) → 200"
_assert_json_eq '.categories | type' array '/categories.categories is an array'
CATCOUNT=$(printf '%s' "$CURL_BODY" | jq '.categories | length')
if [ "$CATCOUNT" -gt 0 ]; then
	echo "  --- /categories has $CATCOUNT entry/entries; per-item shape ---"
	_assert_json_eq '.categories[0].index | type' number '/categories[0].index is numeric'
	_assert_json_eq '.categories[0].name  | type' string '/categories[0].name is string'
	_assert_json_eq '.categories[0].priority | test("^(very_low|low|normal|high|release|auto)$")' \
		true '/categories[0].priority is a known enum value'
fi

# --- 4. /preferences ----------------------------------------------
_curl "$HOST/api/v0/preferences"
_assert_status 401 "GET /preferences (no creds) → 401"

_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/preferences"
_assert_status 200 "GET /preferences (admin) → 200"
# Bare object (no envelope) per Q3 — preferences is a single resource.
_assert_json_eq '.snapshot_at | type' null \
	'/preferences has no snapshot_at envelope (bare object)'

_assert_json_eq '.general.nickname             | type' string  '/preferences.general.nickname is string'
_assert_json_eq '.general.user_hash | length'                          32   '/preferences.general.user_hash is 32-char hex'
_assert_json_eq '.general.check_new_version    | type' boolean '/preferences.general.check_new_version is boolean'

_assert_json_eq '.connection.tcp_port          | type' number  '/preferences.connection.tcp_port is numeric'
_assert_json_eq '.connection.udp_port          | type' number  '/preferences.connection.udp_port is numeric'
_assert_json_eq '.connection.udp_disabled      | type' boolean '/preferences.connection.udp_disabled is boolean'
_assert_json_eq '.connection.network_ed2k      | type' boolean '/preferences.connection.network_ed2k is boolean'
_assert_json_eq '.connection.network_kad       | type' boolean '/preferences.connection.network_kad is boolean'
_assert_json_eq '.connection.autoconnect       | type' boolean '/preferences.connection.autoconnect is boolean'
_assert_json_eq '.connection.max_sources_per_file | type' number '/preferences.connection.max_sources_per_file is numeric'

# ip2country config category (#440). Field types are always present even
# on a GeoIP-less daemon (supported=false, strings empty); source is one
# of the known enum values.
_assert_json_eq '.ip2country.supported       | type' boolean '/preferences.ip2country.supported is boolean'
_assert_json_eq '.ip2country.enabled         | type' boolean '/preferences.ip2country.enabled is boolean'
_assert_json_eq '.ip2country.source | test("^(dbip|maxmind|custom)$")' \
	true '/preferences.ip2country.source is a known enum value'
_assert_json_eq '.ip2country.custom_url      | type' string  '/preferences.ip2country.custom_url is string'
_assert_json_eq '.ip2country.maxmind_license | type' string  '/preferences.ip2country.maxmind_license is string'
_assert_json_eq '.ip2country.auto_update     | type' boolean '/preferences.ip2country.auto_update is boolean'
_assert_json_eq '.ip2country.loaded_source   | type' string  '/preferences.ip2country.loaded_source is string'
_assert_json_eq '.ip2country.db_path         | type' string  '/preferences.ip2country.db_path is string'
_assert_json_eq '.ip2country.db_loaded       | type' boolean '/preferences.ip2country.db_loaded is boolean'
_assert_json_eq '.ip2country.downloading     | type' boolean '/preferences.ip2country.downloading is boolean'
_assert_json_eq '.ip2country.last_result     | type' string  '/preferences.ip2country.last_result is string'

# --- Method gate. ----------------------------------------------
for ep in servers kad categories preferences; do
	_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/$ep"
	_assert_status 405 "DELETE /api/v0/$ep → 405"
done

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
