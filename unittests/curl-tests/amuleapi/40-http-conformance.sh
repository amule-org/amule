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

BODY_FILE=$(mktemp -t amuleapi_40_body.XXXXXX)
HDR_FILE=$(mktemp -t amuleapi_40_hdr.XXXXXX)
BIG_FILE=$(mktemp -t amuleapi_40_big.XXXXXX)
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
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start it first."
fi

TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?include_token=true" | jq -r .token)
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
	python3 - "$HOST" "$1" "${2:-}" <<'PYEOF'
import socket, sys
hostport, path = sys.argv[1], sys.argv[2]
extra = sys.argv[3] if len(sys.argv) > 3 else ""
host, _, port = hostport.partition(":")
try:
    s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=10)
    req = "HEAD %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n" % path
    if extra:
        req += extra + "\r\n"
    req += "\r\n"
    s.sendall(req.encode())
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

# The SSE endpoint is the one unbounded body on the surface, and it does
# not go through the normal response path: the streaming dispatch writes
# its own head and then pushes frames until the peer leaves. A HEAD there
# asks what a GET would answer with, so it must be answered and closed
# rather than streamed, or the "no content on any status" guarantee has a
# hole in exactly the worst place.
#
# It is answered BELOW the auth gate, so it is not a way around it. An
# earlier cut short-circuited above the gate and handed an unauthenticated
# HEAD a 200 where GET answers 401, skipping the auth-failure rate bucket
# too -- so the unauthenticated case is asserted first, and deliberately.
if command -v python3 >/dev/null 2>&1; then
	read -r anon_status _anon_len _anon_bytes <<<"$(_head_probe /api/v0/events)"
	_assert_eq "401" "$anon_status" "HEAD /events without credentials -> 401 (no auth bypass)"
	read -r ev_status _ev_len ev_bytes <<<"$(_head_probe /api/v0/events "Authorization: Bearer $TOKEN")"
	_assert_eq "200" "$ev_status" "HEAD /events with credentials -> 200"
	_assert_eq "0" "$ev_bytes" "HEAD /events does not stream content"
else
	_skip "HEAD /events check (python3 unavailable)"
fi

# The same gate, cross-checked through curl so a regression shows up as a
# plain status mismatch even if the socket probe is skipped.
_assert_eq "401" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$HOST/api/v0/events")" \
	"GET /events without credentials -> 401"

# ---------------------------------------------------------------------------
# Query parameters: unparseable and out-of-range are both 400, never a default
# ---------------------------------------------------------------------------
# Each parameter used to be parsed by hand and each site picked its own rule, so
# the same typo was a hard error on one parameter and a silent behaviour change
# on its neighbour -- `interval=abc` was a 400 while `width=abc` was a 200 that
# quietly meant "everything", on the same endpoint. These assert the wiring: the
# unit tests pin ParseBoundedUint/ParseBoolValue, but only a live request shows
# that a handler routes through them.
_qp() {
	# $1 = query, $2 = expected status, $3 = label
	_assert_eq "$2" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
		"${AUTH[@]}" "$HOST/api/v0/$1")" "$3"
}

# Enumerations: one vocabulary, and anything outside it is answerable.
_qp "downloads?status=active"    200 "status=active -> 200"
_qp "downloads?status=all"       200 "status=all -> 200"
_qp "downloads?status=completed" 200 "status=completed -> 200"
_qp "downloads?status=maybe"     400 "status=maybe -> 400, not a silent default"

# The boolean `status` replaced is refused, not ignored: a caller left on the
# old spelling is told so instead of silently getting the default slice.
_qp "downloads?include_completed=1" 400 "include_completed=1 -> 400 (replaced by status=)"

# Counts: garbage and out-of-range are the same answer.
_qp "downloads?limit=abc"   400 "limit=abc -> 400"
_qp "downloads?limit=1000000001" 400 "limit above the 1e9 ceiling -> 400 (never a silent clamp)"
_qp "downloads?limit=500"   200 "limit=500 -> 200 (the cap itself is valid)"
_qp "downloads?limit="      400 "limit= (empty) -> 400, not an omission"

# Two numeric parameters on one endpoint used to disagree with each other.
_qp "stats/graphs/download_speed?interval=abc" 400 "interval=abc -> 400"
_qp "stats/graphs/download_speed?width=abc"    400 "width=abc -> 400 (was a silent 200)"
_qp "stats/graphs/download_speed?width=99999"  400 "width=99999 -> 400 (was a silent clamp)"
_qp "stats/graphs/download_speed?interval=0"   400 "interval=0 -> 400 (below the documented minimum)"

# The log tail clamped silently too.
_qp "logs/amule?tail=abc"    400 "tail=abc -> 400"
_qp "logs/amule?tail=999999" 400 "tail=999999 -> 400 (was a silent clamp to 100000)"

# ---------------------------------------------------------------------------
# Trailing slash: `/x/` names the same resource as `/x`
# ---------------------------------------------------------------------------
# The two spellings used to disagree by route kind, not by meaning. A literal
# route is compared with `==` so `/status/` simply missed and answered "no such
# endpoint"; a capture route matched with the capture bound to the empty string,
# so `/clients/` reached the handler and was rejected there -- as a 400, for a
# URL that names no resource, while an empty `{hash}` was a 404 for an equally
# meaningless one.
#
# These assert the wiring, which the unit tests for StripTrailingSlash and the
# empty-capture guard cannot: that the dispatcher actually applies the rule.
for p in status version downloads clients shared servers friends; do
	bare=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "${AUTH[@]}" "$HOST/api/v0/$p")
	slash=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "${AUTH[@]}" "$HOST/api/v0/$p/")
	_assert_eq "$bare" "$slash" "/$p and /$p/ answer alike ($bare)"
done

# The static fallthrough is deliberately not normalised -- there a trailing
# slash is a directory rather than a spelling -- so the rule must not have
# leaked outside the API prefix.
_assert_eq "200" "$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$HOST/")" \
	"the static root still answers 200"

# /events reaches the dispatcher only on a method the streaming resolver
# declined, and with no route there it used to fall through to the catch-all
# 404 -- reporting that the endpoint does not exist, on the one resource a
# client is most likely to probe, and escaping the Allow sweep entirely.
curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" \
	-X POST "$HOST/api/v0/events" >/dev/null
_assert_eq "405" "$(awk 'NR==1{print $2}' "$HDR_FILE" | tr -d '\r')" \
	"POST /events -> 405 (not 404)"
EV_ALLOW=$(_hdr Allow)
case "$EV_ALLOW" in
*GET*HEAD*|*HEAD*GET*) _pass "405 on /events carries Allow ($EV_ALLOW)" ;;
*) _fail "405 on /events carries Allow" "got [$EV_ALLOW]" ;;
esac

# HEAD and GET on the stream must agree on the headers they share. They are
# produced by two different writers -- the regular response path and the SSE
# head path -- which each carried their own copy of the header-merge logic,
# and two rounds of fixes landed in one copy but not the other: the stream
# kept advertising keep-alive on a socket the server always closes, and kept
# overwriting Vary instead of appending to it. Comparing the two is what
# notices a fix that only reached one writer.
if command -v python3 >/dev/null 2>&1; then
	# $2 selects the encoding. The identity path is a separate branch in
	# the SSE head writer, and pinning both methods to gzip is why the
	# first cut of this probe could not see a Vary token that was emitted
	# only when the stream was actually compressed.
	_stream_hdr() {
		python3 - "$HOST" "$1" "$TOKEN" "${2:-gzip}" <<'PYEOF'
import socket, sys
hostport, method, tok = sys.argv[1], sys.argv[2], sys.argv[3]
enc = sys.argv[4] if len(sys.argv) > 4 else "gzip"
host, _, port = hostport.partition(":")
try:
    s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=8)
    enc_line = "" if enc == "identity" else ("Accept-Encoding: %s\r\n" % enc)
    s.sendall(("%s /api/v0/events HTTP/1.1\r\nHost: x\r\n%s"
               "Authorization: Bearer %s\r\n\r\n" % (method, enc_line, tok)).encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
except Exception:
    print("")
    raise SystemExit(0)
head = data.partition(b"\r\n\r\n")[0].decode("latin-1")
out = []
for line in head.split("\r\n"):
    low = line.lower()
    # Deliberately wide. Narrowing this to connection/vary is why three
    # consecutive rounds of /events header findings were invisible to the
    # probe that exists to pin them: the divergence was in Content-Encoding
    # and Content-Type, which the filter dropped.
    if low.split(":")[0] in (
            "connection", "vary", "content-encoding", "content-type",
            "cache-control", "x-accel-buffering", "transfer-encoding"):
        out.append(low.replace(" ", ""))
print("|".join(sorted(out)))
PYEOF
	}
	SSE_HEAD=$(_stream_hdr HEAD gzip)
	SSE_GET=$(_stream_hdr GET gzip)
	# ...and again with no encoding requested, which takes the other branch.
	SSE_HEAD_ID=$(_stream_hdr HEAD identity)
	SSE_GET_ID=$(_stream_hdr GET identity)
	if [ -z "$SSE_HEAD" ] || [ -z "$SSE_GET" ]; then
		_skip "SSE HEAD/GET header agreement (probe did not complete)"
	else
		_assert_eq "$SSE_GET" "$SSE_HEAD" \
			"HEAD and GET on /events agree on Connection and Vary (compressed)"
		if [ -z "$SSE_HEAD_ID" ] || [ -z "$SSE_GET_ID" ]; then
			_skip "SSE identity-path header agreement (probe did not complete)"
		else
			_assert_eq "$SSE_GET_ID" "$SSE_HEAD_ID" \
				"HEAD and GET on /events agree on Connection and Vary (uncompressed)"
			case "$SSE_GET_ID" in
			*vary:*accept-encoding*) _pass "an uncompressed stream still varies on Accept-Encoding" ;;
			*) _fail "an uncompressed stream still varies on Accept-Encoding" "got [$SSE_GET_ID]" ;;
			esac
		fi
		case "$SSE_GET" in
		*connection:close*) _pass "the event stream advertises Connection: close" ;;
		*) _fail "the event stream advertises Connection: close" "got [$SSE_GET]" ;;
		esac
	fi
else
	_skip "SSE header agreement check (python3 unavailable)"
fi

# The same comparison under an encoding preference. HEAD has to describe what
# the equivalent GET would return, which with gzip means the SAME
# Content-Encoding and the SAME Content-Length -- diverging binds one strong
# ETag to two codings and lets every probe invalidate a cached gzip response.
# The Content-Length check above sends no Accept-Encoding, so it cannot see
# this; skipping compression for HEAD once passed the whole suite.
#
# Probed against /preferences, NOT /downloads. The transport only compresses
# a body over kGzipMinBodyBytes, and an empty daemon's /downloads is a ~46
# byte envelope -- under the floor, never compressed, so every coding
# assertion below would fail on a bring-up run for a reason that has nothing
# to do with codings. /preferences is a fixed settings schema, kilobytes
# regardless of what the daemon is doing.
CODING_TARGET="$HOST/api/v0/preferences"
# ...and assert that premise instead of trusting it: if this body ever drops
# under the floor the block stops testing what it claims to, and an
# un-suffixed ETag would read as "the coding marker is missing".
CODING_BYTES=$(curl -s --max-time 10 -H "Accept-Encoding: identity" \
	"${AUTH[@]}" "$CODING_TARGET" | wc -c | tr -d ' ')
# -ge, not -gt: WillCompressBody() compresses at `body_size >= 256`, so a
# body of exactly 256 bytes IS compressed and a -gt guard would call it
# unusable. A guard that disagrees with the code it guards is worse than no
# guard, because it is believed.
if [ "${CODING_BYTES:-0}" -ge 256 ]; then
	_pass "the coding probe's body is over the compression floor ($CODING_BYTES bytes)"
else
	_fail "coding probe target is compressible" \
		"/preferences is $CODING_BYTES bytes, under the 256-byte floor"
fi

_enc_triplet() {
	curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "$@" \
		-H "Accept-Encoding: gzip" "${AUTH[@]}" "$CODING_TARGET" >/dev/null
	printf '%s|%s|%s' "$(_hdr Content-Encoding)" "$(_hdr Content-Length)" "$(_hdr ETag)"
}
GZ_HEAD=$(_enc_triplet --head)
GZ_GET=$(_enc_triplet)
_assert_eq "$GZ_GET" "$GZ_HEAD" \
	"HEAD and GET agree on Content-Encoding, Content-Length and ETag under gzip"

# The validator must NAME the representation, not just accompany it. A strong
# ETag identifies one representation, and the hash is taken before
# compression, so without a coding marker the gzip and identity forms of a
# body share a validator -- and a cache holding one can revalidate it for a
# client that cannot accept it. Asserted in both directions so deleting the
# marker fails rather than silently passing.
GZ_ETAG=$(curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
	-H "Accept-Encoding: gzip" "${AUTH[@]}" "$CODING_TARGET" >/dev/null; _hdr ETag)
ID_ETAG=$(curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
	-H "Accept-Encoding: identity" "${AUTH[@]}" "$CODING_TARGET" >/dev/null; _hdr ETag)
case "$GZ_ETAG" in
*-gzip\"*) _pass "the gzip representation's ETag names its coding ($GZ_ETAG)" ;;
*) _fail "gzip ETag names its coding" "got [$GZ_ETAG]" ;;
esac
case "$ID_ETAG" in
*-gzip\"*) _fail "identity ETag does not name a coding" "got [$ID_ETAG]" ;;
*) _pass "the identity representation's ETag carries no coding marker" ;;
esac
if [ "$GZ_ETAG" = "$ID_ETAG" ]; then
	_fail "the two codings have distinct validators" "both are $GZ_ETAG"
else
	_pass "the two codings have distinct validators"
fi

# ...and a 304 must echo the SAME validator the equivalent 200 sent. A 304
# has no body for the transport to compress, so the coding has to be decided
# before the body is dropped; when it was not, a client that cached the gzip
# form was handed the identity ETag back and could never match its stored
# response again.
for pair in "gzip:$GZ_ETAG" "identity:$ID_ETAG"; do
	enc=${pair%%:*}
	want=${pair#*:}
	[ -z "$want" ] && continue
	got=$(curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
		-H "Accept-Encoding: $enc" -H "If-None-Match: $want" \
		"${AUTH[@]}" "$CODING_TARGET" >/dev/null; _hdr ETag)
	code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
		-H "Accept-Encoding: $enc" -H "If-None-Match: $want" \
		"${AUTH[@]}" "$CODING_TARGET")
	_assert_eq "304" "$code" "a $enc client revalidating its own validator gets 304"
	_assert_eq "$want" "$got" "the $enc 304 echoes the validator the 200 sent"
done

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
	"${AUTH[@]}" -X PATCH "$HOST/api/v0/share_directories" >/dev/null
ALLOW_DIRS=$(_hdr Allow)
for m in GET HEAD POST PUT DELETE; do
	case "$ALLOW_DIRS" in
	*"$m"*) _pass "Allow on /share_directories names $m" ;;
	*) _fail "Allow on /share_directories names $m" "got [$ALLOW_DIRS]" ;;
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
# PUT /api/v0/share_directories is a real route (the replace-the-whole-list
# form). It was missing from the advertised list, so a browser doing a
# cross-origin PUT there was told the method is not allowed and blocked the
# request before it was ever sent -- reachable from curl, unreachable from a
# page.
curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 -X OPTIONS \
	-H "Origin: http://example.invalid" \
	-H "Access-Control-Request-Method: PUT" \
	"$HOST/api/v0/share_directories" >/dev/null
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
# Guarded like the other python3 probes: a runner without it should skip
# these checks, not abort the phase at exit 2 and silently drop everything
# that follows.
BIG_READY=0
if ! command -v python3 >/dev/null 2>&1; then
	_skip "413 check (python3 unavailable)"
elif python3 -c "import sys; sys.stdout.write('{\"password\":\"' + 'a'*2200000 + '\"}')" \
	> "$BIG_FILE" 2>/dev/null && [ -s "$BIG_FILE" ]; then
	BIG_READY=1
else
	# Skip AND stay out of the probe below: running it against a
	# truncated file turns one skip into a spurious failure. BIG_FILE is
	# deliberately left set so the EXIT trap still removes it.
	_skip "413 check (could not build the oversize body)"
fi
if [ "$BIG_READY" -eq 1 ]; then
BIG_STATUS=$(curl -s -o "$BODY_FILE" -w '%{http_code}' --max-time 20 \
	-X POST -H "Content-Type: application/json" \
	--data-binary @"$BIG_FILE" "$HOST/api/v0/auth/login" 2>/dev/null || echo "000")
_assert_eq "413" "$BIG_STATUS" "a 2 MiB body -> 413 (not a silent close)"
if [ "$BIG_STATUS" = "413" ]; then
	_assert_eq "payload_too_large" \
		"$(jq -r '.error.code' < "$BODY_FILE" 2>/dev/null)" \
		"413 carries the payload_too_large code"
fi
fi

# Oversized headers -> 431, and a HEAD rejected there ships no content
# either. Both were untested, which is how the 431 branch stayed dead: the
# read buffer was smaller than the parser's header_limit, so an oversized
# header overflowed the buffer before the limit could fire and the
# connection closed with nothing written. A legal header near the cap must
# still succeed, which is the half that catches an over-tight buffer.
if command -v python3 >/dev/null 2>&1; then
	_hdr_probe() {
		python3 - "$HOST" "$1" "$2" <<'PYEOF'
import socket, sys
hostport, method, padlen = sys.argv[1], sys.argv[2], int(sys.argv[3])
host, _, port = hostport.partition(":")
try:
    s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=15)
    pad = ("X-Pad: " + "a" * padlen + "\r\n") if padlen else ""
    s.sendall(("%s /api/v0/version HTTP/1.1\r\nHost: x\r\n%sConnection: close\r\n\r\n"
               % (method, pad)).encode())
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
except Exception:
    print("000 0")
    raise SystemExit(0)
head, _, body = data.partition(b"\r\n\r\n")
status = head.split(b"\r\n")[0].split(b" ")[1].decode() if head else "000"
print("%s %d" % (status, len(body)))
PYEOF
	}
	read -r big_status big_bytes <<<"$(_hdr_probe GET 20000)"
	_assert_eq "431" "$big_status" "oversized request headers -> 431 (not a silent close)"
	read -r bigh_status bigh_bytes <<<"$(_hdr_probe HEAD 20000)"
	_assert_eq "431" "$bigh_status" "HEAD with oversized headers -> 431"
	_assert_eq "0" "$bigh_bytes" "HEAD with oversized headers ships no content"
	read -r ok_status _ok_bytes <<<"$(_hdr_probe GET 12000)"
	_assert_eq "200" "$ok_status" "a legal ~12 KiB header block is still accepted"
else
	_skip "header-limit checks (python3 unavailable)"
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

# ...and a HEAD that times out carries no content either. The probe above
# only ever used GET, so a HEAD shipping the 408 envelope as content -- the
# same violation fixed for 413 and 431 -- had nothing watching it.
if command -v python3 >/dev/null 2>&1; then
	HEAD_408=$(python3 - "$HOST" <<'PYEOF' 2>/dev/null || echo "PYFAIL"
import socket, sys
host, _, port = sys.argv[1].partition(":")
s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=30)
s.sendall(b"HEAD /api/v0/version HTTP/1.1\r\nHost: x\r\nX-Drip: ")
s.settimeout(25)
data = b""
try:
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
except socket.timeout:
    pass
s.close()
head, _, body = data.partition(b"\r\n\r\n")
status = head.split(b"\r\n")[0].split(b" ")[1].decode() if head else "000"
print("%s %d" % (status, len(body)))
PYEOF
	)
	case "$HEAD_408" in
	PYFAIL) _skip "HEAD 408 body check (python3 probe unavailable)" ;;
	"408 0") _pass "HEAD on an unterminated request -> 408 with no content" ;;
	*) _fail "HEAD on an unterminated request -> 408 with no content" "got [$HEAD_408]" ;;
	esac
else
	_skip "HEAD 408 body check (python3 unavailable)"
fi

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
		# ...and that 304 must not advertise a media type it is not
		# carrying. Response::content_type defaults to application/json,
		# so an unset one made a 304 for index.html claim to be JSON.
		curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
			-H "If-None-Match: $GET_ETAG" "$HOST/index.html" >/dev/null
		if _has_hdr Content-Type; then
			_fail "static 304 omits Content-Type" \
				"got [$(_hdr Content-Type)] on a 304 for index.html"
		else
			_pass "static 304 omits Content-Type"
		fi
		# The full If-None-Match grammar, not just an exact match. A
		# reverse proxy that gzips in front of us rewrites the
		# validator to the weak form, and `*` plus comma-separated
		# lists are both legal. The outer layer used to supply this
		# grammar for static assets; now that it stands aside whenever
		# a handler set its own ETag, this path has to speak it itself.
		# A static asset sets its own ETag, so the dispatcher stands
		# aside and the TRANSPORT is what marks the coding on it. That
		# is a different code path from the API routes above, and
		# deleting the transport's stamp leaves those green -- this is
		# the assertion that notices.
		SGZ=$(curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
			-H "Accept-Encoding: gzip" "$HOST/index.html" >/dev/null; _hdr ETag)
		SGZ_ENC=$(_hdr Content-Encoding)
		if [ "$SGZ_ENC" != "gzip" ]; then
			# NOT a skip. index.html is text/html and comfortably
			# over the compression floor, so an uncompressed answer
			# here means the gzip path stopped working -- and a skip
			# would report that as a pass.
			_fail "a static HTML asset is served gzipped when asked" \
				"Content-Encoding was [$SGZ_ENC]"
		else
			case "$SGZ" in
			*-gzip\"*) _pass "a gzipped static asset's ETag names its coding ($SGZ)" ;;
			*) _fail "gzipped static asset ETag names its coding" "got [$SGZ]" ;;
			esac
			SGZ_304=$(curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
				-H "Accept-Encoding: gzip" -H "If-None-Match: $SGZ" \
				"$HOST/index.html" >/dev/null; _hdr ETag)
			_assert_eq "$SGZ" "$SGZ_304" \
				"a gzipped static asset's 304 echoes the validator its 200 sent"
		fi

		STAR=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
			-H "If-None-Match: *" "$HOST/index.html")
		_assert_eq "304" "$STAR" "If-None-Match: * on a static asset -> 304"
		WEAK=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
			-H "If-None-Match: W/$GET_ETAG" "$HOST/index.html")
		_assert_eq "304" "$WEAK" "a weak W/ validator on a static asset -> 304"
		LIST=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
			-H "If-None-Match: \"nomatch\", $GET_ETAG" "$HOST/index.html")
		_assert_eq "304" "$LIST" "an If-None-Match list hitting on the 2nd entry -> 304"
	fi
fi

# --- 7. A changed body never revalidates as unchanged. ---------------
#
# /stats/* is not memoized at all -- eligibility is opt-in and covers only
# /downloads and /shared -- so this asserts the general contract rather than
# the memo: a validator must change whenever the body does. It caught the
# original defect back when the memo did cover this route, and it still
# guards the property that mattered. Historically the key was a timestamp,
# which counts
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

# --- 7b. A per-principal document never shares a validator. ----------
#
# /auth/session's body depends on who is asking, so it is not memo-eligible:
# the key carries no principal. Back when eligibility was an exclusion list
# and this route was not on it, an admin and a guest
# hitting this inside one snapshot second got the SAME validator for two
# different documents, and the second was answered 304 against the first's.
GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"${GUEST_PASS:-guestpass}\"}" \
	"$HOST/api/v0/auth/login?include_token=true" 2>/dev/null | jq -r .token)
if [ -z "$GUEST_TOKEN" ] || [ "$GUEST_TOKEN" = "null" ]; then
	_skip "per-principal ETag check (no guest password configured)"
else
	curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" \
		"$HOST/api/v0/auth/session" >/dev/null
	ADMIN_SESS_ETAG=$(_hdr ETag)
	ADMIN_ROLE=$(jq -r '.role // ""' < "$BODY_FILE" 2>/dev/null)
	curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 \
		-H "Authorization: Bearer $GUEST_TOKEN" \
		"$HOST/api/v0/auth/session" >/dev/null
	GUEST_SESS_ETAG=$(_hdr ETag)
	GUEST_ROLE=$(jq -r '.role // ""' < "$BODY_FILE" 2>/dev/null)
	if [ "$ADMIN_ROLE" = "$GUEST_ROLE" ]; then
		_skip "per-principal ETag check (both logins resolved to '$ADMIN_ROLE')"
	elif [ "$ADMIN_SESS_ETAG" = "$GUEST_SESS_ETAG" ]; then
		_fail "two principals never share a session validator" \
			"admin ($ADMIN_ROLE) and guest ($GUEST_ROLE) both got $ADMIN_SESS_ETAG"
	else
		_pass "two principals get different /auth/session validators"
		# ...and the guest is not 304'd against the admin's.
		CROSS=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
			-H "Authorization: Bearer $GUEST_TOKEN" \
			-H "If-None-Match: $ADMIN_SESS_ETAG" "$HOST/api/v0/auth/session")
		_assert_eq "200" "$CROSS" \
			"a guest is not served 304 against an admin's session validator"
	fi
fi

# --- 7c. A mutation always changes the validator. --------------------
#
# The memo used to be keyed on (target, snapshot_at), and snapshot_at is
# stamped only by the background refresher loop -- never by the inline
# refresh a mutating handler runs. So a PATCH changed the body while the key
# stood still, and the next conditional GET was answered 304 for content
# that had just changed. Measured at 19 of 20 attempts before the key moved
# to a per-refresh revision.
PREFS="$HOST/api/v0/preferences"
curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" "$PREFS" >/dev/null
PREF_ETAG=$(_hdr ETag)
PREF_BEFORE=$(cat "$BODY_FILE")
ORIG_UP=$(printf '%s' "$PREF_BEFORE" | jq -r '.connection.max_upload_kbps // 0' 2>/dev/null)
if [ -z "$PREF_ETAG" ] || [ -z "$ORIG_UP" ]; then
	_skip "mutation-changes-validator check (no ETag or no readable preferences)"
else
	NEW_UP=$((ORIG_UP + 7))
	curl -s -o /dev/null --max-time 10 "${AUTH[@]}" -X PATCH \
		-H "Content-Type: application/json" \
		-d "{\"connection\":{\"max_upload_kbps\":$NEW_UP}}" "$PREFS" >/dev/null
	curl -s -o "$BODY_FILE" -D "$HDR_FILE" --max-time 10 "${AUTH[@]}" "$PREFS" >/dev/null
	PREF_AFTER=$(cat "$BODY_FILE")
	PREF_ETAG2=$(_hdr ETag)
	if [ "$PREF_BEFORE" = "$PREF_AFTER" ]; then
		_skip "mutation-changes-validator check (the PATCH did not change the body)"
	elif [ "$PREF_ETAG" = "$PREF_ETAG2" ]; then
		_fail "a mutation changes the validator" \
			"body changed but the ETag stayed $PREF_ETAG"
	else
		_pass "a mutation changes the validator"
		# ...and the pre-mutation validator must no longer satisfy a
		# conditional GET.
		STALE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "${AUTH[@]}" \
			-H "If-None-Match: $PREF_ETAG" "$PREFS")
		_assert_eq "200" "$STALE" \
			"a pre-mutation validator is not answered 304"
	fi
	# put it back
	curl -s -o /dev/null --max-time 10 "${AUTH[@]}" -X PATCH \
		-H "Content-Type: application/json" \
		-d "{\"connection\":{\"max_upload_kbps\":$ORIG_UP}}" "$PREFS" >/dev/null
fi

# --- 7d. An authenticated body is not shared cache material. ---------
#
# Authenticate() takes a bearer token OR a session cookie, and the WebUI uses
# the cookie -- so those requests carry no Authorization header and RFC 9111
# 3.5's shared-cache prohibition never engages. With no explicit expiry a
# shared cache may keep the 200 under heuristic freshness, and with Cookie
# absent from Vary two users share a cache key: one user's download list can
# be served to another.
for cred in "Authorization: Bearer $TOKEN" "Cookie: amuleapi_token=$TOKEN"; do
	label=${cred%%:*}
	curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 -H "$cred" \
		"$HOST/api/v0/downloads" >/dev/null
	# `private`, deliberately NOT `no-store`: no-store forbids the client's
	# own cache too, so nothing would ever hold an entry to revalidate and
	# no authenticated route would see an If-None-Match at all -- which
	# throws away the validator machinery this suite spends most of its
	# assertions on.
	_assert_eq "private" "$(_hdr Cache-Control)" \
		"an authenticated response ($label) is private (and not no-store)"
	case "$(_hdr Vary)" in
	*Cookie*) _pass "an authenticated response ($label) varies on Cookie" ;;
	*) _fail "authenticated Vary names Cookie ($label)" "got [$(_hdr Vary)]" ;;
	esac
done

# ...and an unauthenticated public probe stays cacheable, or it loses the
# conditional GET the ETag exists for.
curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "$HOST/api/v0/health" >/dev/null
if [ -z "$(_hdr Cache-Control)" ]; then
	_pass "an unauthenticated probe is left cacheable"
else
	_fail "unauthenticated probe left cacheable" "got Cache-Control: $(_hdr Cache-Control)"
fi

# A handler that sets its own policy keeps it: the flag artwork is public and
# long-lived, and blanket-stamping would have made every WebUI icon uncacheable.
curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "$HOST/flags/us.png" >/dev/null
if [ "$(awk 'NR==1{print $2}' "$HDR_FILE" | tr -d '\r')" = "200" ]; then
	_assert_eq "public, max-age=86400" "$(_hdr Cache-Control)" \
		"a handler's own cache policy survives the authenticated stamp"
else
	_skip "flag-artwork cache policy (no flag asset served)"
fi

# ...and the WebUI shell specifically, which is the asset that had NO policy
# of its own and so inherited the authenticated default. Probing /flags alone
# could not see that: it is the one static route that always had one.
#
# Asserted as the PROPERTY, not as a header spelling. An earlier cut matched
# the prefix `public*` and passed against `public, max-age=86400,
# must-revalidate` -- which grants a browser 24 hours of reuse WITHOUT
# asking. These filenames carry no content hash, so that is the window in
# which an upgraded daemon keeps serving the old shell, and in which a new
# shell can be paired with an old bundle, since each asset expires on its
# own clock. must-revalidate does not close it: RFC 9111 5.2.2.2 makes it
# govern what a cache may do once an entry is ALREADY stale. A prefix match
# could not have caught that; what has to hold is that the response grants
# no reuse without revalidation.
_assert_revalidates_every_time() {
	local policy=$1 label=$2
	case "$policy" in
	*no-store*) _pass "$label refuses storage entirely ($policy)" ;;
	*no-cache*) _pass "$label revalidates on every use ($policy)" ;;
	*max-age=0*) _pass "$label revalidates on every use ($policy)" ;;
	*max-age=*)
		_fail "$label revalidates on every use" \
			"[$policy] grants reuse without asking, and these filenames carry no content hash" ;;
	"") _fail "$label revalidates on every use" "no Cache-Control at all" ;;
	*) _fail "$label revalidates on every use" "got [$policy]" ;;
	esac
}
curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 "$HOST/index.html" >/dev/null
if [ "$(awk 'NR==1{print $2}' "$HDR_FILE" | tr -d '\r')" = "200" ]; then
	_assert_revalidates_every_time "$(_hdr Cache-Control)" "the WebUI shell"
	# It must still be shared-cacheable, which takes more than the absence
	# of `private`. RFC 9111 3.5 bars a shared cache from reusing a
	# response to a request that carried an Authorization header unless
	# the response is marked public, must-revalidate or s-maxage --
	# no-cache is not on that list. The WebUI logs in by cookie, but the
	# same shell is fetched with a bearer token below, so without `public`
	# a proxy has to hold a per-user copy of a file identical for every
	# user. Asserted as the token, because that is what the RFC keys on.
	SHELL_CC=$(_hdr Cache-Control)
	# All three of 3.5's directives accept, not just `public`: a
	# no-cache, must-revalidate policy is equally compliant, and a check
	# whose accepting arm is narrower than its own failure message would
	# reject one while telling the reader it carries none of them.
	case "$SHELL_CC" in
	*private*) _fail "the shell is shared-cacheable" "marked private: [$SHELL_CC]" ;;
	*public*|*must-revalidate*|*s-maxage*) _pass "the shell is shared-cacheable ($SHELL_CC)" ;;
	*) _fail "the shell is shared-cacheable" \
		"[$SHELL_CC] carries none of public/must-revalidate/s-maxage, so RFC 9111 3.5 bars reuse" ;;
	esac
	# ...including when the request carries credentials, which is the case
	# that used to stamp it private and re-fetch it per session.
	curl -s -o /dev/null -D "$HDR_FILE" --max-time 10 \
		-H "Authorization: Bearer $TOKEN" "$HOST/index.html" >/dev/null
	SHELL_AUTH_CC=$(_hdr Cache-Control)
	_assert_revalidates_every_time "$SHELL_AUTH_CC" "the shell under credentials"
	# This is the request RFC 9111 3.5 is actually about: it carried an
	# Authorization header, so one of its three directives is what decides
	# whether a shared cache may serve one stored copy to everyone. Same
	# widened arm as above, for the same reason.
	case "$SHELL_AUTH_CC" in
	*private*) _fail "the shell stays shared-cacheable under credentials" \
		"marked private: [$SHELL_AUTH_CC]" ;;
	*public*|*must-revalidate*|*s-maxage*) _pass "the shell stays shared-cacheable under credentials ($SHELL_AUTH_CC)" ;;
	*) _fail "the shell stays shared-cacheable under credentials" \
		"[$SHELL_AUTH_CC] carries none of public/must-revalidate/s-maxage" ;;
	esac
else
	_skip "WebUI shell cache policy (no static frontend served)"
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
