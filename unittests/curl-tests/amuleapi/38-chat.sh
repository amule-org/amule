#!/usr/bin/env bash
#
# amuleapi 38-chat — the /chats surface (issue #971).
#
# Chat is served from a session store in amuled that every client shares,
# so a message sent from the desktop GUI, from amulegui or through this
# API lands in one transcript.
#
# Wire contract:
#   GET    /chats                   → list envelope of conversations
#   GET    /chats/{peer}/messages   → { peer, messages[], total, last_msg_id }
#   POST   /chats/{peer}/messages   → 202 + the created message
#   DELETE /chats/{peer}            → 200 { ok, peer }
#   POST   /friends/{ecid}/messages → 202 (reaches an OFFLINE friend)
#   POST   /clients/{ecid}/messages → 202
#
# `{peer}` is "<ip>:<port>". Every route answers 503 ec_unsupported when
# the connected amuled predates the chat ops.
#
# The peer used below is in the RFC 5737 TEST-NET-3 documentation range,
# which is not routable: the daemon creates the conversation and queues
# the message without anything leaving the host. That is the point --
# POST is 202 ("queued on the peer connection"), never a delivery
# receipt, so an unreachable peer is a valid and deliberate test target.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}

# TEST-NET-3 (RFC 5737). Never routable, never a real peer.
PEER_IP=203.0.113.42
PEER_PORT=4662
PEER="$PEER_IP:$PEER_PORT"

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_38_chat_body.XXXXXX)
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

command -v jq >/dev/null 2>&1 || _die "jq is required."
curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null \
	|| _die "amuleapi at $HOST is not reachable."

echo "amuleapi 38-chat smoke @ $HOST"

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "login failed"

# --- 1. Capability gate. ------------------------------------------
#
# Against an amuled that predates the chat ops every route must answer
# 503 ec_unsupported rather than sending an opcode the daemon would land
# in its unknown-opcode branch (which asserts before it can answer).
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats"
if [ "$CURL_STATUS" = "503" ]; then
	CODE=$(printf '%s' "$CURL_BODY" | jq -r '.error.code')
	if [ "$CODE" = "ec_unsupported" ]; then
		_pass "GET /chats → 503 ec_unsupported (daemon predates the chat ops)"
		echo "    info: connected amuled does not serve chat; remaining checks skipped"
		echo
		echo "OK: $TEST_COUNT/$TEST_COUNT passed"
		exit 0
	fi
	_fail "GET /chats 503 shape" "expected error.code=ec_unsupported, got $CODE"
fi
_assert_status 200 "GET /chats → 200"
_assert_json_eq '.chats | type' array '/chats .chats is array'

# --- 2. Send creates the conversation. ----------------------------
#
# No 404 for an unknown peer: the core creates the session, so POST
# doubles as "start a chat with this address".
_curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d '{"text":"curl-test hello"}' "$HOST/api/v0/chats/$PEER/messages"
_assert_status 202 "POST /chats/{peer}/messages → 202 Accepted"
_assert_json_eq '.ok'                  true    'send reports ok'
_assert_json_eq '.peer'                "$PEER" 'send echoes the conversation key'
_assert_json_eq '.message.direction'   out     'sent message is direction=out'
_assert_json_eq '.message.text'        "curl-test hello" 'send echoes the text'
FIRST_ID=$(printf '%s' "$CURL_BODY" | jq -r '.message.id')

# The refresher mirrors the store on its next tick (1 s).
sleep 3

# --- 3. The conversation is now on the list. ----------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats"
_assert_status 200 "GET /chats → 200 after send"
FOUND=$(printf '%s' "$CURL_BODY" | jq --arg p "$PEER" '[.chats[] | select(.peer == $p)] | length')
if [ "$FOUND" = "1" ]; then
	_pass "/chats lists the new conversation"
else
	_fail "/chats conversation presence" "expected exactly 1 row for $PEER, got $FOUND"
fi
# Narrow the body to just this conversation's row for the field checks.
ROW=$(printf '%s' "$CURL_BODY" | jq --arg p "$PEER" -c '.chats[] | select(.peer == $p)')
printf '%s' "$ROW" > "$CURL_BODY_FILE"; CURL_BODY=$ROW
_assert_json_eq '.ip'                 "$PEER_IP"   'row carries the split ip'
_assert_json_eq '.port'               "$PEER_PORT" 'row carries the split port'
_assert_json_eq '.online'             false        'unrouted peer is offline'
_assert_json_eq '.client_ecid'        0            'offline peer has client_ecid 0'
_assert_json_eq '.last_message.text'  "curl-test hello" 'row carries last_message'
# The core has no nickname for a peer that never answered, so the row must
# fall back to the desktop's own rendering rather than an empty string.
_assert_json_eq '.name' "IP: $PEER_IP Port: $PEER_PORT" 'name falls back to the address form'

# --- 4. Reading the transcript. -----------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/$PEER/messages"
_assert_status 200 "GET /chats/{peer}/messages → 200"
_assert_json_eq '.peer'            "$PEER" 'messages echo the conversation key'
_assert_json_eq '.messages | type' array   'messages is an array'
_assert_json_eq '.messages[0].direction' out 'first message is direction=out'

# Every message object is exactly {id, direction, text, timestamp}.
WRONG=$(printf '%s' "$CURL_BODY" | jq \
	'[.messages[] | select((keys | sort) != ["direction","id","text","timestamp"])] | length')
if [ "$WRONG" = "0" ]; then
	_pass "every message object is {id, direction, text, timestamp}"
else
	_fail "message object shape" "$WRONG messages carry unexpected keys"
fi

# --- 5. since_id is a safe cursor. --------------------------------
#
# ids are monotonic per daemon process, so a client polling with the
# highest id it holds must never see a duplicate and never skip one.
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/chats/$PEER/messages?since_id=$FIRST_ID"
_assert_status 200 "GET messages?since_id → 200"
_assert_json_eq '.messages | length' 0 'since_id at the head returns nothing new'

_curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d '{"text":"second message"}' "$HOST/api/v0/chats/$PEER/messages"
_assert_status 202 "POST second message → 202"
sleep 3
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/chats/$PEER/messages?since_id=$FIRST_ID"
_assert_json_eq '.messages | length'   1                'since_id returns exactly the new message'
_assert_json_eq '.messages[0].text'    "second message" 'since_id returns the right message'

# --- 5b. `tail`, not `limit`. --------------------------------------
#
# This selects the last N of the window rather than a page of it, which is
# what the log endpoints already call `tail`. It was called `limit`, which
# on nine other collections means a window paired with `offset` -- one word
# with two meanings is a rule a client has to learn twice.
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/chats/$PEER/messages?tail=1"
_assert_status 200 "GET messages?tail=1 → 200"
_assert_json_eq '.messages | length' 1 'tail=1 returns just the newest message'
_assert_json_eq '.messages[0].text' "second message" 'tail keeps the newest, not the oldest'

# The old spelling is now an unknown parameter, which is simply ignored --
# it is not a count the endpoint honours under another name.
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/chats/$PEER/messages?limit=1"
_assert_status 200 "GET messages?limit=1 → 200 (unknown param, ignored)"
_assert_json_eq '.messages | length' 2 'limit no longer truncates the chat window'

# Same strict parsing as every other count on the surface.
_curl -H "Authorization: Bearer $TOKEN" \
	"$HOST/api/v0/chats/$PEER/messages?tail=abc"
_assert_status 400 "GET messages?tail=abc → 400"

# --- 6. Input validation. -----------------------------------------
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/notanaddress/messages"
_assert_status 400 "GET messages with a malformed {peer} → 400"
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/$PEER_IP:99999/messages"
_assert_status 400 "GET messages with an out-of-range port → 400"
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/198.51.100.7:4662/messages"
_assert_status 404 "GET messages for an unknown conversation → 404"
_curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d '{"text":""}' "$HOST/api/v0/chats/$PEER/messages"
_assert_status 400 "POST with empty text → 400"
_curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d '{}' "$HOST/api/v0/chats/$PEER/messages"
_assert_status 400 "POST with no text field → 400"
_curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d '{"text":"x"}' "$HOST/api/v0/clients/4294967290/messages"
_assert_status 404 "POST to an unknown client ECID → 404"
_curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
	-d '{"text":"x"}' "$HOST/api/v0/friends/4294967290/messages"
_assert_status 404 "POST to an unknown friend ECID → 404"

# --- 7. Method gating. --------------------------------------------
_curl -X PUT -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats"
_assert_status 405 "PUT /chats → 405"
_curl -X PATCH -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/$PEER"
_assert_status 405 "PATCH /chats/{peer} → 405"

# --- 8. Guests read but do not write. -----------------------------
GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" \
	"$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
if [ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ]; then
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/chats"
	_assert_status 200 "GET /chats as guest → 200 (read-only data)"
	_curl -X POST -H "Authorization: Bearer $GUEST_TOKEN" -H "Content-Type: application/json" \
		-d '{"text":"nope"}' "$HOST/api/v0/chats/$PEER/messages"
	_assert_status 403 "POST as guest → 403 (sending is admin-only)"
	_curl -X DELETE -H "Authorization: Bearer $GUEST_TOKEN" "$HOST/api/v0/chats/$PEER"
	_assert_status 403 "DELETE as guest → 403"
else
	echo "    info: guest login unavailable; guest checks skipped"
fi

# --- 9. Closing is global and actually removes it. ----------------
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/$PEER"
_assert_status 200 "DELETE /chats/{peer} → 200"
_assert_json_eq '.ok'   true    'close reports ok'
_assert_json_eq '.peer' "$PEER" 'close echoes the conversation key'
sleep 3
_curl -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats"
GONE=$(printf '%s' "$CURL_BODY" | jq --arg p "$PEER" '[.chats[] | select(.peer == $p)] | length')
if [ "$GONE" = "0" ]; then
	_pass "closed conversation is gone from /chats"
else
	_fail "close removal" "conversation still listed after DELETE"
fi
_curl -X DELETE -H "Authorization: Bearer $TOKEN" "$HOST/api/v0/chats/$PEER"
_assert_status 404 "DELETE an already-closed conversation → 404"

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
