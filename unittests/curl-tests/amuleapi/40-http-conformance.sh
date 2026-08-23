#!/usr/bin/env bash
#
# amuleapi 40-http-conformance - the wire-protocol contract, as opposed to
# any one endpoint's payload. Covers the things a conformant HTTP client (or
# a plain proxy) relies on and that no per-endpoint smoke was watching:
#
#   * HEAD carries no content, on ANY status, and still reports the
#     Content-Length a GET would return
#   * the read-side limits answer with a typed envelope instead of closing
#     the connection without a word
#   * 405 carries the Allow header RFC 9110 requires
#   * bodiless replies (204 / 304) omit Content-Type rather than sending an
#     empty one
#   * a static asset reports ONE validator, whichever method asked, and
#     honours a lowercase if-none-match
#   * the CORS preflight advertises every method the route actually serves
#   * a conditional GET is never answered 304 for a body that has changed
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-test &
#   ./40-http-conformance.sh
#
# Environment:
#   HOST=localhost:4713   amuleapi endpoint (default port)
#   ADMIN_PASS=adminpass  admin password
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}

FAIL_COUNT=0
TEST_COUNT=0
# Skips are counted apart from TEST_COUNT, never folded into it: a skipped
# check is coverage that did not happen, and adding it to the passed tally
# would report the absence of a check as a check that succeeded.
SKIP_COUNT=0

BODY_FILE=$(mktemp -t amuleapi_39_body.XXXXXX)
HDR_FILE=$(mktemp -t amuleapi_39_hdr.XXXXXX)
BIG_FILE=$(mktemp -t amuleapi_39_big.XXXXXX)
trap 'rm -f "$BODY_FILE" "$HDR_FILE" "$BIG_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}

_assert_eq() {
	local expected=$1 actual=$2 label=$3
	if [ "$expected" = "$actual" ]; then
		_pass "$label"
	else
		_fail "$label" "expected [$expected], got [$actual]"
	fi
}

# Header value by name, case-insensitively, from the last -D dump.
_hdr() {
	tr -d '\r' < "$HDR_FILE" | awk -v want="$(printf '%s' "$1" | tr 'A-Z' 'a-z')" \
		'BEGIN{FS=": "} tolower($1)==want {sub(/^[^:]*: /,""); print; exit}'
}

# Is a header present at all (regardless of value)?
_has_hdr() {
	tr -d '\r' < "$HDR_FILE" | awk -v want="$(printf '%s' "$1" | tr 'A-Z' 'a-z')" \
		'BEGIN{FS=":"} tolower($1)==want {found=1} END{exit !found}'
}

if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/version" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start it first."
fi

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$TOKEN" ] && [ "$TOKEN" != "null" ] || _die "admin login failed"
AUTH=(-H "Authorization: Bearer $TOKEN")

echo "amuleapi 40-http-conformance smoke @ $HOST"

# --- 1. HEAD carries no content, on any status. ----------------------
#
# The body strip used to sit inside the 200-only branch, so every HEAD that
# ended in 4xx or 5xx shipped the JSON error envelope as content. On a
# keep-alive connection that is not cosmetic: a client that correctly stops
# reading after the headers leaves those bytes in the socket and starts
# parsing the next response mid-JSON.
# Probed on a raw socket, not with curl: curl knows a HEAD response has no
# body and will not read one, so it reports zero bytes whether or not the
# server actually sent them -- which is exactly the bug being tested.
# Prints "<status> <content-length> <bytes-after-header-block>".
_head_probe() {
	python3 - "$HOST" "$1" <<'PYEOF'
import socket, sys
hostport, path = sys.argv[1], sys.argv[2]
host, _, port = hostport.partition(":")
try:
    s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=10)
    s.sendall(("HEAD %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % path).encode())
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
except Exception:
    print("000 - -")
    raise SystemExit(0)
head, sep, body = data.partition(b"\r\n\r\n")
status = head.split(b"\r\n")[0].split(b" ")[1].decode() if head else "000"
clen = "-"
for line in head.decode("latin-1").split("\r\n"):
    if line.lower().startswith("content-length:"):
        clen = line.split(":", 1)[1].strip()
print("%s %s %d" % (status, clen, len(body)))
PYEOF
}

if ! command -v python3 >/dev/null 2>&1; then
	_skip "HEAD wire-level checks (python3 unavailable)"
else
	for probe in "/api/v0/nope:404" "/api/v0/status:401"; do
		url=${probe%:*}
		want=${probe##*:}
		read -r got_status _got_len got_bytes <<<"$(_head_probe "$url")"
		_assert_eq "$want" "$got_status" "HEAD $url -> $want"
		_assert_eq "0" "$got_bytes" "HEAD $url puts no content on the wire"
	done

	# 200 too, and there Content-Length must describe what a GET returns
	# rather than the zero bytes HEAD writes -- otherwise the header is
	# useless to a client sizing a fetch.
	read -r v_status v_len v_bytes <<<"$(_head_probe /api/v0/version)"
	_assert_eq "200" "$v_status" "HEAD /version -> 200"
	_assert_eq "0" "$v_bytes" "HEAD /version puts no content on the wire"
	GET_LEN=$(curl -s --max-time 10 "$HOST/api/v0/version" | wc -c | tr -d ' ')
	_assert_eq "$GET_LEN" "$v_len" "HEAD /version Content-Length matches the GET body"
fi

# --- 2. 405 carries Allow. -------------------------------------------
#
# RFC 9110 §15.5.6 makes the header mandatory. The accepted methods were
# only ever in the human-readable message, which generic tooling and
# capability discovery do not read.
curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 \
	"${AUTH[@]}" -X DELETE "$HOST/api/v0/status" >/dev/null
_assert_eq "405" "$(awk 'NR==1{print $2}' "$HDR_FILE" | tr -d '\r')" \
	"DELETE /status -> 405"
ALLOW=$(_hdr Allow)
if [ -z "$ALLOW" ]; then
	_fail "405 carries an Allow header" "header absent on DELETE /status"
else
	_pass "405 carries an Allow header (Allow: $ALLOW)"
	# HEAD is served wherever GET is, and the header is the machine-readable
	# list, so it must say so even though the prose message says "only GET".
	case "$ALLOW" in
	*GET*) _pass "Allow on /status names GET" ;;
	*) _fail "Allow on /status names GET" "got [$ALLOW]" ;;
	esac
	case "$ALLOW" in
	*HEAD*) _pass "Allow on /status names HEAD" ;;
	*) _fail "Allow on /status names HEAD" "got [$ALLOW]" ;;
	esac
fi

# A route with a richer verb set reports all of it.
curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 \
	"${AUTH[@]}" -X PATCH "$HOST/api/v0/shared/directories" >/dev/null
ALLOW_DIRS=$(_hdr Allow)
for m in GET HEAD POST PUT DELETE; do
	case "$ALLOW_DIRS" in
	*"$m"*) _pass "Allow on /shared/directories names $m" ;;
	*) _fail "Allow on /shared/directories names $m" "got [$ALLOW_DIRS]" ;;
	esac
done

# --- 3. Bodiless replies omit Content-Type. --------------------------
#
# The handlers clear it for 204 and the ETag layer clears it for 304, but
# the writer used to emit the header anyway with an empty value. A
# Content-Type whose value is not a media type is malformed.
ETAG=$(curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" \
	"$HOST/api/v0/version" >/dev/null; _hdr ETag)
if [ -z "$ETAG" ]; then
	_skip "304 Content-Type check (no ETag on /version)"
else
	curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" \
		-H "If-None-Match: $ETAG" "$HOST/api/v0/version" >/dev/null
	_assert_eq "304" "$(awk 'NR==1{print $2}' "$HDR_FILE" | tr -d '\r')" \
		"If-None-Match on the current ETag -> 304"
	if _has_hdr Content-Type; then
		_fail "304 omits Content-Type" "header present: [$(_hdr Content-Type)]"
	else
		_pass "304 omits Content-Type"
	fi
	# The validator itself must survive: clients re-stamp their cached copy
	# from it (RFC 7232 §4.1).
	if [ -n "$(_hdr ETag)" ]; then
		_pass "304 still carries the ETag"
	else
		_fail "304 still carries the ETag" "header absent"
	fi
fi

# The CORS preflight answers 204; same rule.
curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 -X OPTIONS \
	-H "Origin: http://example.invalid" \
	-H "Access-Control-Request-Method: GET" \
	"$HOST/api/v0/version" >/dev/null
PRE_STATUS=$(awk 'NR==1{print $2}' "$HDR_FILE" | tr -d '\r')
if [ "$PRE_STATUS" = "204" ]; then
	if _has_hdr Content-Type; then
		_fail "204 preflight omits Content-Type" "header present: [$(_hdr Content-Type)]"
	else
		_pass "204 preflight omits Content-Type"
	fi
else
	_skip "204 preflight Content-Type check (CORS disabled; preflight answered $PRE_STATUS)"
fi

# --- 4. The CORS preflight advertises every method the route serves. --
#
# PUT /api/v0/shared/directories is a real route (the replace-the-whole-list
# form). It was missing from the advertised list, so a browser doing a
# cross-origin PUT there was told the method is not allowed and blocked the
# request before it was ever sent -- reachable from curl, unreachable from a
# page.
curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 -X OPTIONS \
	-H "Origin: http://example.invalid" \
	-H "Access-Control-Request-Method: PUT" \
	"$HOST/api/v0/shared/directories" >/dev/null
ACAM=$(_hdr Access-Control-Allow-Methods)
if [ -z "$ACAM" ]; then
	_skip "preflight method list (CORS disabled; no Access-Control-Allow-Methods)"
else
	case "$ACAM" in
	*PUT*) _pass "preflight advertises PUT (Access-Control-Allow-Methods: $ACAM)" ;;
	*) _fail "preflight advertises PUT" "got [$ACAM]" ;;
	esac
fi

# --- 5. Read-side limits answer, they do not just hang up. -----------
#
# Every other rejection on this surface is a typed JSON envelope. These
# three used to close the connection with nothing written, so a caller could
# not tell "too large" from "daemon crashed" or "firewall ate it".
python3 -c "import sys; sys.stdout.write('{\"password\":\"' + 'a'*2200000 + '\"}')" \
	> "$BIG_FILE" 2>/dev/null || _die "python3 is required to build the oversize body"
BIG_STATUS=$(curl -s -o "$BODY_FILE" -w '%{http_code}' --max-time 20 \
	-X POST -H "Content-Type: application/json" \
	--data-binary @"$BIG_FILE" "$HOST/api/v0/auth/login" 2>/dev/null || echo "000")
_assert_eq "413" "$BIG_STATUS" "a 2 MiB body -> 413 (not a silent close)"
if [ "$BIG_STATUS" = "413" ]; then
	_assert_eq "payload_too_large" \
		"$(jq -r '.error.code' < "$BODY_FILE" 2>/dev/null)" \
		"413 carries the payload_too_large code"
fi

# Headers that never terminate: the 10 s read timeout should answer 408.
# Hand-rolled because curl will not send a deliberately incomplete request.
TIMEOUT_OUT=$(python3 - "$HOST" <<'PYEOF' 2>/dev/null || echo "PYFAIL"
import socket, sys, time
host, _, port = sys.argv[1].partition(":")
s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=30)
s.sendall(b"GET /api/v0/version HTTP/1.1\r\nHost: x\r\nX-Dangling: ")
s.settimeout(25)
buf = b""
try:
    while b"\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
except socket.timeout:
    pass
s.close()
sys.stdout.write(buf.decode("latin-1").split("\r\n")[0])
PYEOF
)
case "$TIMEOUT_OUT" in
*408*)   _pass "an unterminated request -> 408 (not a silent close)" ;;
PYFAIL)  _skip "408 read-timeout check (python3 socket probe unavailable)" ;;
"")      _fail "an unterminated request -> 408" "connection closed with no response" ;;
*)       _fail "an unterminated request -> 408" "got status line [$TIMEOUT_OUT]" ;;
esac

# --- 6. A static asset has ONE validator. ----------------------------
#
# The handler computed an mtime+size ETag and the outer layer stamped an
# MD5-of-body over the top -- but only when the body was non-empty, which
# was true on GET and false on HEAD. So the two methods handed out
# different validators for the same URL, and touching the file changed one
# while the other stayed put.
ROOT_STATUS=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$HOST/index.html")
if [ "$ROOT_STATUS" != "200" ]; then
	_skip "static-asset validator checks (no static frontend served; /index.html -> $ROOT_STATUS)"
else
	curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 -I "$HOST/index.html" >/dev/null
	HEAD_ETAG=$(_hdr ETag)
	curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "$HOST/index.html" >/dev/null
	GET_ETAG=$(_hdr ETag)
	if [ -z "$HEAD_ETAG" ] || [ -z "$GET_ETAG" ]; then
		_fail "static asset reports an ETag on both methods" \
			"HEAD=[$HEAD_ETAG] GET=[$GET_ETAG]"
	else
		_assert_eq "$GET_ETAG" "$HEAD_ETAG" \
			"HEAD and GET report the same validator for /index.html"
	fi
	# HTTP header names are case-insensitive. The static path used a
	# literal map lookup, so a lowercase if-none-match -- what an
	# HTTP/2-shaped client library produces -- silently lost conditional
	# GET and re-downloaded the asset every time.
	if [ -n "$GET_ETAG" ]; then
		LOWER=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
			-H "if-none-match: $GET_ETAG" "$HOST/index.html")
		_assert_eq "304" "$LOWER" "lowercase if-none-match on a static asset -> 304"
		UPPER=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
			-H "If-None-Match: $GET_ETAG" "$HOST/index.html")
		_assert_eq "304" "$UPPER" "canonical If-None-Match on a static asset -> 304"
	fi
fi

# --- 7. A changed body never revalidates as unchanged. ---------------
#
# The ETag memo is keyed on (target, snapshot_at) and snapshot_at counts
# whole seconds. The stats graphs keep their own 1 s TTL cache, refetched
# out of phase with the refresher tick, so their body can change while the
# key does not -- and the memoized validator was then served for a body it
# was not computed from. RFC 9110 §8.8.1 requires the entity-tag to change
# whenever the representation does; any conformant cache is otherwise
# entitled to keep serving the stale copy.
GRAPH="$HOST/api/v0/stats/graphs/download_speed?width=3"
PREV_BODY=""; PREV_ETAG=""; VIOLATION=""; OBSERVED=0
for _ in $(seq 1 40); do
	curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" "$GRAPH" >/dev/null
	NOW_ETAG=$(_hdr ETag)
	NOW_BODY=$(cat "$BODY_FILE")
	[ -z "$NOW_ETAG" ] && break
	if [ -n "$PREV_ETAG" ] && [ "$NOW_BODY" != "$PREV_BODY" ]; then
		OBSERVED=$((OBSERVED+1))
		if [ "$NOW_ETAG" = "$PREV_ETAG" ]; then
			VIOLATION="body changed while ETag stayed $NOW_ETAG"
			break
		fi
	fi
	PREV_BODY=$NOW_BODY; PREV_ETAG=$NOW_ETAG
	sleep 0.4
done
if [ -z "$PREV_ETAG" ]; then
	_skip "stats-graph validator check (no ETag on $GRAPH)"
elif [ -n "$VIOLATION" ]; then
	_fail "a changed stats-graph body always changes the ETag" "$VIOLATION"
elif [ "$OBSERVED" -eq 0 ]; then
	_skip "stats-graph validator check (body never changed in the sample window)"
else
	_pass "a changed stats-graph body always changes the ETag ($OBSERVED change(s) seen)"
fi

# And the paired direction: a conditional GET must not be told 304 for a
# body that has moved on since the validator was minted.
if [ -n "$PREV_ETAG" ]; then
	curl -s -o "$BODY_FILE" --max-time 10 "${AUTH[@]}" "$GRAPH" >/dev/null
	FRESH=$(cat "$BODY_FILE")
	COND=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "${AUTH[@]}" \
		-H "If-None-Match: $PREV_ETAG" "$GRAPH")
	if [ "$COND" = "304" ] && [ "$FRESH" != "$PREV_BODY" ]; then
		_fail "no 304 for a body that changed" \
			"served 304 against ETag $PREV_ETAG although the body differs"
	else
		_pass "conditional GET on a stats graph does not claim a stale copy is current"
	fi
fi

# --- 8. status.ed2k.server_ip is an address, not an endpoint. --------
#
# The field carried brackets and the port -- "[77.42.68.79:4232]" -- inside
# a field named server_ip, beside a server_port that already held the port.
# A client joining the two got "[77.42.68.79:4232]:4232".
curl -s -o "$BODY_FILE" --max-time 10 "${AUTH[@]}" "$HOST/api/v0/status" >/dev/null
SRV_IP=$(jq -r '.ed2k.server_ip // ""' < "$BODY_FILE" 2>/dev/null)
if [ -z "$SRV_IP" ]; then
	_skip "status.ed2k.server_ip shape (not connected to a server)"
elif printf '%s' "$SRV_IP" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then
	_pass "status.ed2k.server_ip is a bare dotted quad ($SRV_IP)"
else
	_fail "status.ed2k.server_ip is a bare dotted quad" "got [$SRV_IP]"
fi

# --- Summary. -------------------------------------------------------
echo
SKIP_NOTE=""
[ "$SKIP_COUNT" -gt 0 ] && SKIP_NOTE=" ($SKIP_COUNT check(s) skipped)"
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed$SKIP_NOTE"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
