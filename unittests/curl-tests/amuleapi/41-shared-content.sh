#!/usr/bin/env bash
#
# amuleapi 41-shared-content — GET / HEAD /shared/{hash}/content.
#
# Endpoint:
#   GET|HEAD /api/v0/shared/{hash}/content   → 200 / 206 / 304 / 416
#
# Serves the bytes of a COMPLETED shared file straight off the filesystem
# amuleapi is running on. The response is deliberately hostile to the
# browser that receives it:
#
#   Content-Type: application/octet-stream    hard-coded, never sniffed
#   Content-Disposition: attachment; ...      never `inline`
#   X-Content-Type-Options: nosniff
#   Content-Security-Policy: default-src 'none'; sandbox
#
# — because both the bytes and the filename came from strangers on the ed2k
# network (a completed download lands in Incoming, which is itself shared),
# and amuleapi serves the Web UI from this same origin. A shared `evil.html`
# returned as text/html would execute against the user's own session.
#
# Why each status is what it is:
#   200  full representation, or a Range header that was ignored (below)
#   206  a single satisfiable byte range
#   304  If-None-Match matched. Evaluated BEFORE Range, per RFC 9110 13.2.2,
#        so a conditional request carrying a Range answers 304, never 206.
#   401  no bearer token — the share list is not public
#   404  no shared file with that hash. Also every path-resolution refusal,
#        collapsed into the same reply so it cannot be used to probe the
#        share layout.
#   405  anything but GET / HEAD
#   409  partfile_unsupported — a .part file's on-disk offsets do not
#        correspond to the file's own, so a range out of it would be
#        silently WRONG rather than merely unavailable.
#   416  the range starts at or past EOF
#   503  ec_content_unreachable / ec_content_mismatch when amuleapi runs
#        against a REMOTE amuled and the daemon's paths do not exist (or
#        name different files) on this host; path_unavailable while the
#        directory has not yet arrived over EC; file_responses_exhausted
#        when the transport's concurrent-file-response cap is saturated.
#
# The single most important assertion in this file is the multi-range one.
# A multi-range request is answered 200 with the FULL body: RFC 7233 3.1
# permits a server to ignore a Range header, and refusing to ever emit
# multipart/byteranges is what makes the CVE-2011-3192 ("Apache Killer")
# amplification shape a no-op here — a few hundred bytes of Range header
# cannot be turned into hundreds of overlapping windows of one file.
#
# NOT covered here: flat memory across a multi-hundred-MB transfer. The
# transport streams through a fixed 64 KiB buffer instead of buffering the
# body, which is asserted by RangeFileBodyTest under ctest — a 600 MB
# transfer is too slow and too fragile for a smoke run.
#
# Requires AMULE_SHARED_DIR (see run-all.sh) pointing at a directory the
# connected amuled shares, so the script can plant its own fixture rather
# than depend on the operator's library. Without it the script SKIPS.
#
# Usage:
#   amuled -c /tmp/amule-regtest &
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   AMULE_SHARED_DIR=... ./41-shared-content.sh
#
# Exits 0 on success (or a clean skip), 1 on any failed assertion,
# 2 on bring-up error.

set -u
set -o pipefail

HOST=${HOST:-localhost:4713}
ADMIN_PASS=${ADMIN_PASS:-adminpass}
GUEST_PASS=${GUEST_PASS:-guestpass}
AMULE_SHARED_DIR=${AMULE_SHARED_DIR:-}

# 3 MiB: large enough that a suffix range, a 64 KiB streaming buffer and a
# truncated body are all distinguishable, small enough to transfer a dozen
# times in a smoke run.
FIXTURE_NAME=amuleapi-content-regtest.bin
FIXTURE_SIZE=3145728
# Same fixture idea, but with a name carrying the characters that break a
# naively-built Content-Disposition: a double quote, a space and a
# semicolon. Planted best-effort — some filesystems refuse it.
ODD_NAME='amuleapi-content "odd; name.bin'

FAIL_COUNT=0
TEST_COUNT=0

CURL_BODY_FILE=$(mktemp -t amuleapi_41_shared_content_body.XXXXXX)
CURL_HEAD_FILE=$(mktemp -t amuleapi_41_shared_content_head.XXXXXX)
trap 'rm -f "$CURL_BODY_FILE" "$CURL_HEAD_FILE"' EXIT

_die()  { echo "FATAL: $*" >&2; exit 2; }
_pass() { TEST_COUNT=$((TEST_COUNT+1)); echo "  PASS  $1"; }
_fail() {
	TEST_COUNT=$((TEST_COUNT+1)); FAIL_COUNT=$((FAIL_COUNT+1))
	echo "  FAIL  $1"
	shift
	for arg in "$@"; do echo "        $arg"; done
}
_skip() { echo "  SKIP  $1"; }

_curl() {
	local resp
	# Truncate both dumps first. curl does not rewrite -o when the response
	# carries no body, so a 304 would otherwise be asserted against the
	# PREVIOUS response's bytes -- which is the one case this file most
	# needs to get right.
	: > "$CURL_BODY_FILE"
	: > "$CURL_HEAD_FILE"
	resp=$(curl -s --max-time 60 \
		-D "$CURL_HEAD_FILE" \
		-o "$CURL_BODY_FILE" -w '%{http_code}' "$@") \
		|| _die "curl invocation failed for $*"
	CURL_STATUS=$resp
	CURL_BODY=$(cat "$CURL_BODY_FILE")
	CURL_HEAD=$(cat "$CURL_HEAD_FILE")
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

_assert_body_empty() {
	local label=$1
	if [ ! -s "$CURL_BODY_FILE" ]; then
		_pass "$label"
	else
		_fail "$label" "expected an empty body, got $(wc -c < "$CURL_BODY_FILE" | tr -d ' ') bytes"
	fi
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
	tr -d '\r' < "$CURL_HEAD_FILE" | awk -v want="$(printf '%s' "$1" | tr 'A-Z' 'a-z')" \
		'BEGIN{FS=": "} tolower($1)==want {sub(/^[^:]*: /,""); print; exit}'
}

# Is a header present at all (regardless of value)?
_has_hdr() {
	tr -d '\r' < "$CURL_HEAD_FILE" | awk -v want="$(printf '%s' "$1" | tr 'A-Z' 'a-z')" \
		'BEGIN{FS=":"} tolower($1)==want {found=1} END{exit !found}'
}

_assert_hdr_eq() {
	local name=$1 expected=$2 label=$3
	_assert_eq "$expected" "$(_hdr "$name")" "$label"
}

# --- Preconditions. -------------------------------------------------
if ! command -v jq >/dev/null 2>&1; then _die "jq is required."; fi
if ! curl -s -o /dev/null --max-time 2 "$HOST/api/v0/health" 2>/dev/null; then
	_die "amuleapi at $HOST is not reachable. Start amuleapi first."
fi

echo "amuleapi 41-shared-content smoke @ $HOST"

if [ -z "$AMULE_SHARED_DIR" ]; then
	echo
	echo "SKIP: AMULE_SHARED_DIR is unset, so the fixture cannot be planted."
	echo "      Point it at a directory the connected amuled shares (its"
	echo "      Incoming, typically) and re-run. run-all.sh forwards it."
	exit 0
fi
if [ ! -d "$AMULE_SHARED_DIR" ] || [ ! -w "$AMULE_SHARED_DIR" ]; then
	echo
	echo "SKIP: AMULE_SHARED_DIR=$AMULE_SHARED_DIR is not a writable directory."
	exit 0
fi

ADMIN_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$ADMIN_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
[ -n "$ADMIN_TOKEN" ] && [ "$ADMIN_TOKEN" != "null" ] || _die "admin login failed"

GUEST_TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
	-d "{\"password\":\"$GUEST_PASS\"}" "$HOST/api/v0/auth/login?type=bearer" | jq -r .token)
HAVE_GUEST=0
[ -n "$GUEST_TOKEN" ] && [ "$GUEST_TOKEN" != "null" ] && HAVE_GUEST=1

AUTH=(-H "Authorization: Bearer $ADMIN_TOKEN")

# --- Fixture provisioning. ------------------------------------------
# Self-provisioning on purpose: the assertions below compare byte windows
# against a local copy, so the script must own the file it serves rather
# than adopt whatever happens to be in the operator's library. Reused
# across runs once it exists — the bytes are what matter, not their
# freshness.
FIXTURE_PATH="$AMULE_SHARED_DIR/$FIXTURE_NAME"
if [ ! -f "$FIXTURE_PATH" ] || [ "$(wc -c < "$FIXTURE_PATH" | tr -d ' ')" != "$FIXTURE_SIZE" ]; then
	head -c "$FIXTURE_SIZE" /dev/urandom > "$FIXTURE_PATH" \
		|| _die "cannot write fixture to AMULE_SHARED_DIR=$AMULE_SHARED_DIR"
	echo "    info: planted fixture $FIXTURE_PATH ($FIXTURE_SIZE bytes)"
fi

ODD_PATH="$AMULE_SHARED_DIR/$ODD_NAME"
HAVE_ODD=0
if [ -f "$ODD_PATH" ]; then
	HAVE_ODD=1
elif head -c 1024 /dev/urandom > "$ODD_PATH" 2>/dev/null && [ -f "$ODD_PATH" ]; then
	HAVE_ODD=1
	echo "    info: planted header-injection fixture $ODD_PATH"
else
	rm -f "$ODD_PATH" 2>/dev/null
	echo "    info: filesystem refused a filename containing a quote/semicolon;" \
		"the Content-Disposition injection case is skipped"
fi

# Ask amuled to re-walk its shares, then wait for the fixture to be hashed
# in. A cold hash of a few MiB is sub-second; the generous poll is for the
# scheduling latency, not the hashing.
curl -s -o /dev/null -X POST "${AUTH[@]}" "$HOST/api/v0/shared_reload"
TEST_HASH=""
ODD_HASH=""
for _ in $(seq 1 40); do
	sleep 1
	_curl "${AUTH[@]}" "$HOST/api/v0/shared"
	TEST_HASH=$(printf '%s' "$CURL_BODY" \
		| jq -r --arg n "$FIXTURE_NAME" '.shared[] | select(.name == $n) | .hash' | head -1)
	[ -n "$TEST_HASH" ] && break
done
[ -n "$TEST_HASH" ] \
	|| _die "fixture $FIXTURE_NAME planted in $AMULE_SHARED_DIR but never appeared in /shared"

SERVED_SIZE=$(printf '%s' "$CURL_BODY" \
	| jq -r --arg n "$FIXTURE_NAME" '.shared[] | select(.name == $n) | .size' | head -1)
_assert_eq "$FIXTURE_SIZE" "$SERVED_SIZE" "/shared reports the fixture at its on-disk size"

if [ "$HAVE_ODD" = "1" ]; then
	ODD_HASH=$(printf '%s' "$CURL_BODY" \
		| jq -r --arg n "$ODD_NAME" '.shared[] | select(.name == $n) | .hash' | head -1)
	if [ -z "$ODD_HASH" ]; then
		echo "    info: the odd-named fixture is not shared yet; skipping its case"
		HAVE_ODD=0
	fi
fi

# Classify a partfile for the 409 guard, the same sweep 30-shared-verify
# does. Planting a half-finished download is not reproducible in a smoke
# run, so this case is skipped rather than forced.
PART_HASH=""
for h in $(printf '%s' "$CURL_BODY" | jq -r '.shared[].hash' | head -20); do
	INCOMPLETE=$(curl -s --max-time 10 "${AUTH[@]}" \
		"$HOST/api/v0/shared/$h" | jq -r '.incomplete')
	if [ "$INCOMPLETE" = "true" ]; then PART_HASH=$h; break; fi
done

echo "    info: serving hash=$TEST_HASH size=$FIXTURE_SIZE"

CONTENT_URL="$HOST/api/v0/shared/$TEST_HASH/content"

# --- 1. Auth gate. ---------------------------------------------------
_curl "$CONTENT_URL"
_assert_status 401 "GET /shared/{hash}/content (no token) → 401"

# GUEST is enough: reading a shared file's bytes is a read of data the
# daemon is already offering to the whole ed2k network.
if [ "$HAVE_GUEST" = "1" ]; then
	_curl -H "Authorization: Bearer $GUEST_TOKEN" "$CONTENT_URL"
	_assert_status 200 "GET /shared/{hash}/content (guest) → 200"
fi

# --- 2. Unknown hash → 404. ------------------------------------------
_curl "${AUTH[@]}" "$HOST/api/v0/shared/00000000000000000000000000000000/content"
_assert_status 404 "GET /shared/{unknown}/content → 404"
_assert_json_eq '.error.code' not_found "unknown hash → error.code=not_found"

# --- 3. Method gate. -------------------------------------------------
_curl -X POST "${AUTH[@]}" "$CONTENT_URL"
_assert_status 405 "POST /shared/{hash}/content → 405"

_curl -X DELETE "${AUTH[@]}" "$CONTENT_URL"
_assert_status 405 "DELETE /shared/{hash}/content → 405"

# --- 4. Full GET: status, headers, and the exact bytes. --------------
_curl "${AUTH[@]}" "$CONTENT_URL"
_assert_status 200 "GET /shared/{hash}/content → 200"

if cmp -s "$FIXTURE_PATH" "$CURL_BODY_FILE"; then
	_pass "the served body is byte-identical to the file on disk"
else
	_fail "the served body is byte-identical to the file on disk" \
		"on disk: $(wc -c < "$FIXTURE_PATH" | tr -d ' ') bytes," \
		"served: $(wc -c < "$CURL_BODY_FILE" | tr -d ' ') bytes"
fi

_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "Content-Length is the exact file size"
_assert_hdr_eq "Content-Type" "application/octet-stream" \
	"Content-Type is application/octet-stream (never sniffed from the extension)"
_assert_hdr_eq "X-Content-Type-Options" "nosniff" "X-Content-Type-Options: nosniff"
_assert_hdr_eq "Content-Security-Policy" "default-src 'none'; sandbox" \
	"Content-Security-Policy sandboxes the response"
_assert_hdr_eq "Accept-Ranges" "bytes" "Accept-Ranges: bytes advertises range support"
_assert_hdr_eq "Content-Disposition" \
	"attachment; filename=\"$FIXTURE_NAME\"; filename*=UTF-8''$FIXTURE_NAME" \
	"Content-Disposition is attachment with both filename forms"

CONTENT_ETAG=$(_hdr "ETag")
if [ -n "$CONTENT_ETAG" ]; then
	_pass "GET emits a handler-set ETag ($CONTENT_ETAG)"
else
	_fail "GET emits an ETag" "no ETag header in the response"
fi

# --- 5. Single ranges. -----------------------------------------------
_curl "${AUTH[@]}" -H "Range: bytes=0-9" "$CONTENT_URL"
_assert_status 206 "Range: bytes=0-9 → 206"
_assert_hdr_eq "Content-Range" "bytes 0-9/$FIXTURE_SIZE" "first-ten Content-Range"
_assert_hdr_eq "Content-Length" "10" "first-ten Content-Length"
head -c 10 "$FIXTURE_PATH" > "$CURL_HEAD_FILE.expect10"
if cmp -s "$CURL_HEAD_FILE.expect10" "$CURL_BODY_FILE"; then
	_pass "bytes=0-9 returns the first ten bytes of the file"
else
	_fail "bytes=0-9 returns the first ten bytes of the file" "window mismatch"
fi
rm -f "$CURL_HEAD_FILE.expect10"

SUFFIX_START=$((FIXTURE_SIZE - 100))
SUFFIX_END=$((FIXTURE_SIZE - 1))
_curl "${AUTH[@]}" -H "Range: bytes=-100" "$CONTENT_URL"
_assert_status 206 "Range: bytes=-100 (suffix) → 206"
_assert_hdr_eq "Content-Range" "bytes $SUFFIX_START-$SUFFIX_END/$FIXTURE_SIZE" \
	"suffix range resolves against the file size"
_assert_hdr_eq "Content-Length" "100" "suffix Content-Length"
tail -c 100 "$FIXTURE_PATH" > "$CURL_HEAD_FILE.expect100"
if cmp -s "$CURL_HEAD_FILE.expect100" "$CURL_BODY_FILE"; then
	_pass "bytes=-100 returns the last hundred bytes of the file"
else
	_fail "bytes=-100 returns the last hundred bytes of the file" "window mismatch"
fi
rm -f "$CURL_HEAD_FILE.expect100"

# An open-ended range from EOF is unsatisfiable, not empty: there is no
# byte at offset `size`. 416 carries `bytes */size` so the client learns
# the real length without a second request.
_curl "${AUTH[@]}" -H "Range: bytes=$FIXTURE_SIZE-" "$CONTENT_URL"
_assert_status 416 "Range starting at EOF → 416"
_assert_hdr_eq "Content-Range" "bytes */$FIXTURE_SIZE" \
	"416 reports the true length as bytes */size"

# --- 6. Ranges that are IGNORED rather than served. ------------------
# THE important one. Multi-range is answered 200 with the whole body, not
# 206 multipart/byteranges and not 416. RFC 7233 3.1 explicitly allows a
# server to ignore Range, and never emitting multipart is what neutralises
# the CVE-2011-3192 "Apache Killer" amplification: a request naming
# hundreds of overlapping windows costs exactly one linear read of the
# file, the same as no Range header at all.
_curl "${AUTH[@]}" -H "Range: bytes=0-1,5-6" "$CONTENT_URL"
_assert_status 200 "multi-range → 200 with the full body (CVE-2011-3192 is a no-op)"
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "multi-range body is the whole file"
if _has_hdr "Content-Range"; then
	_fail "multi-range emits no Content-Range" "got: $(_hdr Content-Range)"
else
	_pass "multi-range emits no Content-Range"
fi
case "$(_hdr Content-Type)" in
	*multipart*) _fail "multi-range is not multipart/byteranges" "got: $(_hdr Content-Type)" ;;
	*) _pass "multi-range is not multipart/byteranges" ;;
esac

# The same amplification shape at scale: 200 overlapping windows still
# costs one body, which is the whole point of the rule above.
MANY_RANGES=$(awk 'BEGIN{ for (i = 0; i < 200; i++) printf "%s0-%d", (i ? "," : ""), i }')
_curl "${AUTH[@]}" -H "Range: bytes=$MANY_RANGES" "$CONTENT_URL"
_assert_status 200 "200 overlapping ranges → one 200, not 200 windows"
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "the amplification attempt costs exactly one body"

# A unit this server does not implement must be ignored, not rejected —
# RFC 9110 14.2 says an unrecognised range unit is treated as absent.
_curl "${AUTH[@]}" -H "Range: items=0-9" "$CONTENT_URL"
_assert_status 200 "unknown range unit (items=) is ignored → 200 full body"
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "unknown range unit returns the whole file"

# Overflow must be REJECTED, never wrapped. A 30-digit value does not fit
# a 64-bit offset; wrapping it would compute an in-range window from an
# absurd request and read at an offset the client never named.
_curl "${AUTH[@]}" -H "Range: bytes=999999999999999999999999999999-" "$CONTENT_URL"
_assert_status 200 "overflowing range start is rejected → 200 full body"
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "overflowing start does not wrap into a window"

_curl "${AUTH[@]}" -H "Range: bytes=0-999999999999999999999999999999" "$CONTENT_URL"
_assert_status 200 "overflowing range end is rejected → 200 full body"
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "overflowing end does not wrap into a window"

# --- 7. File responses are never deflated. ---------------------------
# The transport refuses to compress a file body, so no amount of
# Accept-Encoding negotiation produces a Content-Encoding. That is also
# why the ETag carries no coding suffix: there is exactly one
# representation to name.
_curl "${AUTH[@]}" -H "Accept-Encoding: gzip, deflate" "$CONTENT_URL"
_assert_status 200 "Accept-Encoding: gzip → 200"
if _has_hdr "Content-Encoding"; then
	_fail "file responses are never deflated" "got Content-Encoding: $(_hdr Content-Encoding)"
else
	_pass "file responses are never deflated (no Content-Encoding)"
fi
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "an uncompressed body keeps its exact length"

# --- 8. Conditional GET. ---------------------------------------------
# Answered by the handler, not by the generic ETag stamping in Dispatch:
# Dispatch stands aside the moment a handler sets its own validator, so a
# route that emits an ETag and does NOT answer If-None-Match hands out a
# validator no client can ever revalidate against.
_curl "${AUTH[@]}" -H "If-None-Match: $CONTENT_ETAG" "$CONTENT_URL"
_assert_status 304 "If-None-Match with the served ETag → 304"
_assert_body_empty "304 carries no body"
_assert_hdr_eq "ETag" "$CONTENT_ETAG" "304 preserves the ETag header"

_curl "${AUTH[@]}" -H "If-None-Match: *" "$CONTENT_URL"
_assert_status 304 "If-None-Match: * → 304"

# Header names are case-insensitive on the wire, and the lookup has to be
# too — an intermediary is free to lowercase them.
_curl "${AUTH[@]}" -H "if-none-match: $CONTENT_ETAG" "$CONTENT_URL"
_assert_status 304 "lowercase if-none-match → 304"

# The weak form is what an nginx in front of amuleapi emits after it
# rewrites the validator.
_curl "${AUTH[@]}" -H "If-None-Match: W/$CONTENT_ETAG" "$CONTENT_URL"
_assert_status 304 "weak validator W/\"...\" → 304"

# A list, the form a client holding several cached variants sends.
_curl "${AUTH[@]}" -H "If-None-Match: \"nope-1\", $CONTENT_ETAG, \"nope-2\"" "$CONTENT_URL"
_assert_status 304 "If-None-Match list containing the ETag → 304"

_curl "${AUTH[@]}" -H "If-None-Match: \"not-the-validator\"" "$CONTENT_URL"
_assert_status 200 "non-matching validator → 200 full body"
_assert_hdr_eq "Content-Length" "$FIXTURE_SIZE" "a failed revalidation returns the whole file"

# RFC 9110 13.2.2: preconditions are evaluated BEFORE Range, so a matching
# If-None-Match wins outright and the Range is never reached. Answering
# 206 here would hand a client that already holds the bytes a window it
# did not need, and would do it under a validator that says nothing
# changed.
_curl "${AUTH[@]}" -H "If-None-Match: $CONTENT_ETAG" -H "Range: bytes=0-9" "$CONTENT_URL"
_assert_status 304 "If-None-Match + Range → 304, not 206 (precondition first)"
_assert_body_empty "the 304 that pre-empts a Range still carries no body"

# --- 9. HEAD, on a raw socket. ---------------------------------------
# curl KNOWS a HEAD response has no body and will not read one, so it
# reports zero bytes whether or not the server actually wrote them —
# which is exactly the bug worth testing. Same technique as
# 40-http-conformance. Prints "<status> <content-length> <body-bytes>".
_raw_probe() {
	python3 - "$HOST" "$@" <<'PYEOF'
import socket, sys
hostport, method, path = sys.argv[1], sys.argv[2], sys.argv[3]
# Every remaining argument is one extra header line. Joined with CRLF here
# rather than passed as one pre-joined blob, so a caller cannot accidentally
# smuggle a bare LF into the request and get a silent connection failure.
extra = [h for h in sys.argv[4:] if h]
host, _, port = hostport.partition(":")
try:
    s = socket.create_connection((host or "localhost", int(port or 4713)), timeout=30)
    req = "%s %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n" % (method, path)
    for h in extra:
        req += h + "\r\n"
    req += "\r\n"
    s.sendall(req.encode())
    data = b""
    while True:
        chunk = s.recv(65536)
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
	_skip "wire-level HEAD / 304 checks (python3 unavailable)"
else
	CONTENT_PATH="/api/v0/shared/$TEST_HASH/content"
	AUTH_HDR="Authorization: Bearer $ADMIN_TOKEN"

	read -r h_status h_len h_bytes <<<"$(_raw_probe "HEAD" "$CONTENT_PATH" "$AUTH_HDR")"
	_assert_eq "200" "$h_status" "HEAD /shared/{hash}/content → 200"
	_assert_eq "0" "$h_bytes" "HEAD puts no content on the wire"
	# The whole point of a HEAD here is sizing a download, so the length
	# must describe what a GET would return, not the zero bytes written.
	_assert_eq "$FIXTURE_SIZE" "$h_len" "HEAD Content-Length is what a GET would return"

	read -r hu_status _hu_len _hu_bytes <<<"$(_raw_probe "HEAD" "$CONTENT_PATH")"
	_assert_eq "401" "$hu_status" "HEAD without credentials → 401 (no auth bypass)"

	# A 304 must be bodiless on the wire too. On a keep-alive connection a
	# stray body would be parsed as the head of the next response.
	read -r c_status _c_len c_bytes \
		<<<"$(_raw_probe "GET" "$CONTENT_PATH" "$AUTH_HDR" \
			"If-None-Match: $CONTENT_ETAG")"
	_assert_eq "304" "$c_status" "raw GET with a matching If-None-Match → 304"
	_assert_eq "0" "$c_bytes" "304 puts no content on the wire"
fi

# --- 10. Concurrent-file-response cap. -------------------------------
# The transport caps concurrent file responses (kMaxConcurrentFileResponses,
# HttpServer.cpp) because each one pins a file descriptor and a streaming
# buffer for as long as the peer takes to drain it. Over the cap the answer
# is an honest 503 with a Retry-After, not a queue that grows without
# bound. Probed on raw sockets that read only the status line and then hold
# the connection: a curl fan-out would finish each transfer too quickly to
# overlap reliably.
if ! command -v python3 >/dev/null 2>&1; then
	_skip "concurrent-file-response cap (python3 unavailable)"
else
	CAP_RESULT=$(python3 - "$HOST" "/api/v0/shared/$TEST_HASH/content" "$ADMIN_TOKEN" <<'PYEOF'
import socket, sys
hostport, path, token = sys.argv[1], sys.argv[2], sys.argv[3]
host, _, port = hostport.partition(":")
host = host or "localhost"
port = int(port or 4713)
socks, statuses, bodies = [], [], []
try:
    for _ in range(9):
        s = socket.create_connection((host, port), timeout=30)
        s.sendall((
            "GET %s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\n"
            "Connection: close\r\n\r\n" % (path, token)).encode())
        socks.append(s)
    # Read only the header block from each, leaving any 200 body unread so
    # the response stays open and the cap stays saturated.
    for s in socks:
        buf = b""
        while b"\r\n\r\n" not in buf and len(buf) < 65536:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        line = head.split(b"\r\n")[0].decode("latin-1") if head else ""
        statuses.append(line.split(" ")[1] if len(line.split(" ")) > 1 else "000")
        bodies.append((head.decode("latin-1"), rest))
finally:
    for s in socks:
        try:
            s.close()
        except Exception:
            pass
ok = statuses.count("200")
busy = statuses.count("503")
retry_after = "no"
code = "-"
for head, rest in bodies:
    if head.split("\r\n")[0].split(" ")[1:2] == ["503"]:
        if "retry-after:" in head.lower():
            retry_after = "yes"
        if "file_responses_exhausted" in rest.decode("latin-1", "replace"):
            code = "file_responses_exhausted"
        break
print("%d %d %s %s" % (ok, busy, retry_after, code))
PYEOF
)
	read -r cap_ok cap_busy cap_retry cap_code <<<"$CAP_RESULT"
	if [ "$cap_ok" -gt 0 ] && [ "$cap_busy" -gt 0 ] && [ $((cap_ok + cap_busy)) -eq 9 ]; then
		_pass "9 simultaneous file requests split into ${cap_ok}x200 + ${cap_busy}x503"
	else
		_fail "9 simultaneous file requests hit the concurrency cap" \
			"got ${cap_ok}x200 + ${cap_busy}x503 out of 9"
	fi
	_assert_eq "yes" "$cap_retry" "the over-cap 503 carries a Retry-After header"
	_assert_eq "file_responses_exhausted" "$cap_code" \
		"the over-cap 503 reports error.code=file_responses_exhausted"
fi

# --- 11. Content-Disposition cannot be steered by the filename. ------
# The filename comes from the ed2k network, so it is attacker-authored. A
# quote closes the quoted-string form and a CR/LF would split the header
# outright; neither may survive into the response.
if [ "$HAVE_ODD" = "1" ]; then
	_curl "${AUTH[@]}" "$HOST/api/v0/shared/$ODD_HASH/content"
	_assert_status 200 "GET content of a file whose name carries quote/semicolon → 200"
	ODD_CD=$(_hdr "Content-Disposition")
	case "$ODD_CD" in
		attachment\;*) _pass "the odd name still yields an attachment disposition" ;;
		*) _fail "the odd name still yields an attachment disposition" "got: $ODD_CD" ;;
	esac
	# The danger is the quote CLOSING the quoted-string early and turning
	# the rest of the filename into forged parameters. A semicolon inside
	# the quoted string is legal and must be left alone, so the check is
	# where the string ends, not which characters it contains: whatever
	# follows the closing quote has to be the filename* parameter this
	# server emits, and nothing else.
	CD_AFTER_OPEN=${ODD_CD#*filename=\"}
	CD_TAIL=${CD_AFTER_OPEN#*\"}
	case "$CD_TAIL" in
		"; filename*=UTF-8''"*)
			_pass "the embedded quote does not terminate the quoted filename early" ;;
		*) _fail "the embedded quote does not terminate the quoted filename early" \
			"after the closing quote: [$CD_TAIL]" "full header: $ODD_CD" ;;
	esac
	# And the extended form percent-encodes it rather than emitting it raw.
	case "$ODD_CD" in
		*"filename*=UTF-8''"*%22*)
			_pass "filename* percent-encodes the quote (%22)" ;;
		*) _fail "filename* percent-encodes the quote" "got: $ODD_CD" ;;
	esac
	# One header line, not two: a smuggled CR/LF would show up as extra
	# lines in the dump under the same name.
	CD_LINES=$(tr -d '\r' < "$CURL_HEAD_FILE" | grep -ci '^content-disposition:')
	_assert_eq "1" "$CD_LINES" "the odd name produces exactly one Content-Disposition line"
else
	_skip "Content-Disposition injection case (no odd-named fixture available)"
fi

# --- 12. Partfile guard. ---------------------------------------------
# A partfile's on-disk layout is gapped and its offsets do not correspond
# to the completed file's, so any window out of it would be silently wrong.
# Refused outright rather than served partially.
if [ -n "$PART_HASH" ]; then
	_curl "${AUTH[@]}" "$HOST/api/v0/shared/$PART_HASH/content"
	_assert_status 409 "GET content of a partfile → 409"
	_assert_json_eq '.error.code' partfile_unsupported \
		"partfile → error.code=partfile_unsupported"
else
	_skip "partfile 409 guard (no incomplete shared file in the library)"
fi

# --- Summary. -----------------------------------------------------
echo
if [ "$FAIL_COUNT" -eq 0 ]; then
	echo "OK: $TEST_COUNT/$TEST_COUNT passed"
	exit 0
fi
echo "FAIL: $FAIL_COUNT/$TEST_COUNT failed"
exit 1
