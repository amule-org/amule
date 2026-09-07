#!/usr/bin/env bash
#
# amuleapi 43-client-protocol-extensions — the peer capability word on the
# REST surface.
#
# amuled publishes a peer's eMuleAI vendor capability word over EC as
# EC_TAG_CLIENT_MOD_CAPABILITIES, and the desktop GUI renders it as its
# "Protocol extensions" row. Without this field the EC surface and the REST
# surface disagree about what is known of a peer, and a headless caller has
# no way to see the word at all.
#
# What is asserted here:
#
#   * `protocol_extensions` is present on the /clients list row, on the
#     /clients/{ecid} detail object, and in the client_* SSE payloads, so
#     the three views of one peer do not disagree,
#   * it is an array of tokens, not the bitfield the word arrives in: a
#     caller must not have to carry its own copy of aMule's bit meanings,
#     and a list rather than one token because a peer claims any
#     combination of them,
#   * every token is one the daemon defines. The daemon masks the word with
#     MOD_MISCOPT_KNOWN_MASK (0x1F) before it reaches EC, so a token outside
#     the known set means either the mask moved, the table in
#     src/PeerCapabilities.h grew a row the serialiser does not know, or
#     something re-derived the word downstream — which is the failure this
#     field is most likely to have, and the one nothing else would notice,
#   * the key is always present, never omitted: [] is a real answer here
#     (the peer claimed nothing, which nearly every peer does) and a caller
#     must not have to tell it from a missing key.
#
# Note on an empty peer list: a fresh regtest daemon may be connected to no
# peers, so the per-row assertions skip themselves rather than fail. Run
# against a daemon with live sources to exercise them.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   ./43-client-protocol-extensions.sh
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0
SKIP_COUNT=0
AUTH=()

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_curl() {
	local resp
	resp=$(curl -s --max-time 10 -D /tmp/amuleapi_43_head -o /tmp/amuleapi_43_body \
		-w '%{http_code}' "${AUTH[@]}" "$@") || _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat /tmp/amuleapi_43_body)
}

_assert_status() {
	local expected=$1 label=$2
	if [ "$CURL_STATUS" = "$expected" ]; then
		_pass "$label (HTTP $CURL_STATUS)"
	else
		_fail "$label" "expected HTTP $expected, got $CURL_STATUS"
	fi
}

_jq() { echo "$CURL_BODY" | jq -r "$1" 2>/dev/null; }

# The tokens the table in src/PeerCapabilities.h defines, in bit order. A row
# added there without a matching entry here fails the membership check below,
# which is the point: the two must not drift.
KNOWN_TOKENS="extended_source_exchange nat_traversal_utp ipv6 serving_buddy_pull nat_traversal_quic"

# The two properties every appearance of the field has to hold, wherever it
# came from: it is an array, and every token in it is one the daemon defines.
_assert_word() {
	local where=$1 type=$2 value=$3

	if [ "$type" = "array" ]; then
		_pass "$where: protocol_extensions is an array of tokens"
	else
		_fail "$where" "expected an array, got type '$type'"
		return
	fi

	local unknown=""
	local token
	for token in $(echo "$value" | jq -r '.[]' 2>/dev/null); do
		case " $KNOWN_TOKENS " in
			*" $token "*) ;;
			*) unknown="$unknown $token" ;;
		esac
	done

	if [ -z "$unknown" ]; then
		_pass "$where: only defined tokens appear (tokens=$value)"
	else
		_fail "$where" \
			"unknown token(s):$unknown" \
			"the daemon masks in CPeerCapabilities::SetFromWire and names bits in" \
			"CPeerCapabilities::Table(); a token here means the mask moved, the" \
			"table grew a row this test does not know, or the word was re-derived"
	fi
}

trap 'rm -f /tmp/amuleapi_43_head /tmp/amuleapi_43_body /tmp/amuleapi_43_sse' EXIT

command -v jq >/dev/null 2>&1 || _die "jq is required"

if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 43-client-protocol-extensions @ $HOST"

# --- 0. Log in. ----------------------------------------------------
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] \
	|| _die "could not log in for protocol-extension tests"
AUTH=(-H "Authorization: Bearer $TOKEN")

# --- 1. The list row. ----------------------------------------------
_curl "$HOST/api/v0/clients"
_assert_status 200 "GET /clients"

COUNT=$(_jq '.clients | length')
COUNT=${COUNT:-0}
echo "        ($COUNT peer(s) connected)"

if [ "$COUNT" -gt 0 ]; then
	if [ "$(_jq '.clients[0] | has("protocol_extensions")')" = "true" ]; then
		_pass "list row carries protocol_extensions"
		_assert_word "list row" \
			"$(_jq '.clients[0].protocol_extensions | type')" \
			"$(_jq '.clients[0].protocol_extensions')"
	else
		_fail "list row" "no protocol_extensions key in: $(_jq '.clients[0] | keys')"
	fi

	# Present on every row, not only the first: the key is unconditional, so
	# a peer that claimed nothing still reports 0 rather than dropping it.
	MISSING=$(_jq '[.clients[] | select(has("protocol_extensions") | not)] | length')
	if [ "${MISSING:-1}" -eq 0 ]; then
		_pass "every list row carries the key, including peers claiming nothing"
	else
		_fail "list rows" "$MISSING of $COUNT rows omit protocol_extensions"
	fi

	ECID=$(_jq '.clients[0].ecid')
else
	_skip "list row shape (no peer connected)"
	_skip "every list row carries the key (no peer connected)"
	ECID=""
fi

# --- 2. The detail object. -----------------------------------------
if [ -n "$ECID" ] && [ "$ECID" != "null" ]; then
	_curl "$HOST/api/v0/clients/$ECID"
	_assert_status 200 "GET /clients/$ECID"

	if [ "$(_jq 'has("protocol_extensions")')" = "true" ]; then
		_pass "detail object carries protocol_extensions"
		_assert_word "detail object" \
			"$(_jq '.protocol_extensions | type')" \
			"$(_jq '.protocol_extensions')"
	else
		_fail "detail object" "no protocol_extensions key in: $(_jq 'keys')"
	fi

	# The detail object is documented as a superset of the list row, so the
	# two must agree about this peer rather than merely both have the key.
	_curl "$HOST/api/v0/clients"
	LIST_WORD=$(_jq ".clients[] | select(.ecid == $ECID) | .protocol_extensions")
	_curl "$HOST/api/v0/clients/$ECID"
	DETAIL_WORD=$(_jq '.protocol_extensions')
	if [ "$LIST_WORD" = "$DETAIL_WORD" ]; then
		_pass "list and detail report the same word for ecid $ECID ($DETAIL_WORD)"
	else
		_fail "list vs detail" "list says '$LIST_WORD', detail says '$DETAIL_WORD'"
	fi
else
	_skip "detail object shape (no peer connected)"
	_skip "list and detail agree (no peer connected)"
fi

# --- 3. The SSE payload. -------------------------------------------
# client_added / client_updated are documented to carry the same field set as
# the list row. A field added to the REST writer and not to the diff writer
# is exactly the failure that guarantee exists to catch, and it is invisible
# from REST alone.
curl -s --max-time 12 -N "${AUTH[@]}" \
	"$HOST/api/v0/events?filter=client_added,client_updated" \
	> /tmp/amuleapi_43_sse 2>/dev/null &
SSE_PID=$!
sleep 10
kill "$SSE_PID" 2>/dev/null
wait "$SSE_PID" 2>/dev/null

CLIENT_FRAME=$(grep '^data: ' /tmp/amuleapi_43_sse 2>/dev/null \
	| sed 's/^data: //' \
	| jq -c 'select(has("ecid"))' 2>/dev/null \
	| head -1)

if [ -n "$CLIENT_FRAME" ]; then
	if [ "$(echo "$CLIENT_FRAME" | jq -r 'has("protocol_extensions")')" = "true" ]; then
		_pass "client_* payload carries protocol_extensions"
		_assert_word "SSE payload" \
			"$(echo "$CLIENT_FRAME" | jq -r '.protocol_extensions | type')" \
			"$(echo "$CLIENT_FRAME" | jq -r '.protocol_extensions')"
	else
		_fail "SSE payload" \
			"no protocol_extensions in client payload: ${CLIENT_FRAME:0:200}" \
			"the REST row and the diff writer have drifted apart"
	fi
else
	_skip "SSE payload shape (no client event within the listen window)"
	_skip "SSE payload word (no client event within the listen window)"
fi

echo
SKIP_NOTE=""
[ "$SKIP_COUNT" -gt 0 ] && SKIP_NOTE=" ($SKIP_COUNT check(s) skipped)"
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "43-client-protocol-extensions: all $TEST_COUNT assertions passed$SKIP_NOTE"
	exit 0
fi
echo "43-client-protocol-extensions: $FAIL_COUNT of $TEST_COUNT assertions FAILED"
exit 1
