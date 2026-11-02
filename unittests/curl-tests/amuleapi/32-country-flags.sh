#!/usr/bin/env bash
#
# amuleapi 32-country-flags — GET /flags/{code}.png.
#
# The peer / server `country_code` on /clients and /servers is only half
# the story; this route is where a frontend gets the matching artwork.
# The bytes come from the embedded icon table (the same PNGs the desktop
# GUI draws). These assertions cover the public route and caching contract:
#
#   * a known code returns a PNG with the right Content-Type,
#   * the response is cacheable — ETag + Cache-Control — and honours
#     If-None-Match with a 304,
#   * HEAD returns the headers with no body,
#   * the artwork is served verbatim, NOT gzip-encoded (it is already
#     entropy-coded; deflate over a PNG is pure waste),
#   * the "unknown" placeholder (the "??" flag the desktop falls back
#     to for an unresolved code) is reachable under the same route,
#   * anything else that is not exactly two lowercase ASCII letters +
#     an optional @2x / @3x suffix and .png is a 404, including uppercase, wrong length, traversal
#     attempts and a well-formed code the set has no artwork for,
#   * non-safe methods are 405,
#   * it works with `[Server]/StaticRoot` unset — artwork has its own
#     embedded artwork; it never reads files.
#
# Usage:
#   amuleapi --config-dir=/tmp/amuleapi-regtest &
#   ./32-country-flags.sh
#
# Exits 0 on success, 1 on any failed assertion, 2 on bring-up error.

set -u
set -o pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

# PNGs are binary: byte checks read $CURL_BODY_FILE. Body length is
# curl's own %{size_download} ($CURL_SIZE), never the file size, because
# `-I` writes the response headers into the file.

_preflight

echo "amuleapi 32-country-flags @ $HOST"

# --- 1. A known code returns a PNG. -------------------------------
_curl "$HOST/flags/de.png"
_assert_status 200 "GET /flags/de.png"

CT=$(_header content-type)
case "$CT" in
	image/png*) _pass "Content-Type is image/png" ;;
	*) _fail "/flags/de.png Content-Type" "expected image/png, got: ${CT:-<none>}" ;;
esac

# The flag PNG fallbacks are compact raster images. Check the 8-byte PNG signature so a future refactor that returns
# the wrong table entry (or an empty body) fails loudly here.
SIG=$(od -An -tx1 -N8 "$CURL_BODY_FILE" | tr -d ' \n')
if [ "$SIG" = "89504e470d0a1a0a" ]; then
	_pass "body starts with the PNG signature"
else
	_fail "/flags/de.png body" "expected PNG magic 89504e470d0a1a0a, got ${SIG:-<empty>}"
fi

if [ "$CURL_SIZE" -gt 100 ] && [ "$CURL_SIZE" -lt 8192 ]; then
	_pass "body is a plausible flag size ($CURL_SIZE bytes)"
else
	_fail "/flags/de.png size" "expected 100..8192 bytes, got $CURL_SIZE"
fi

# --- 2. Cacheable: ETag + Cache-Control. --------------------------
ETAG=$(_header etag)
if [ -n "$ETAG" ]; then
	_pass "GET /flags/de.png emits an ETag ($ETAG)"
else
	_fail "/flags/de.png ETag" "no ETag header found"
fi

CC=$(_header cache-control)
case "$CC" in
	*max-age=*) _pass "Cache-Control carries a max-age ($CC)" ;;
	*) _fail "/flags/de.png Cache-Control" "expected a max-age directive, got: ${CC:-<none>}" ;;
esac

# --- 3. If-None-Match → 304. --------------------------------------
_curl -H "If-None-Match: $ETAG" "$HOST/flags/de.png"
_assert_status 304 "If-None-Match matching ETag → 304"
if [ "$CURL_SIZE" = "0" ]; then
	_pass "304 carries no body"
else
	_fail "304 body" "expected 0 body bytes, got $CURL_SIZE"
fi

# --- 4. HEAD returns headers, no body. ----------------------------
_curl -I "$HOST/flags/de.png"
_assert_status 200 "HEAD /flags/de.png"
if [ "$CURL_SIZE" = "0" ]; then
	_pass "HEAD carries no body"
else
	_fail "HEAD body" "expected 0 body bytes, got $CURL_SIZE"
fi
# HEAD reports the Content-Length the equivalent GET would return, with
# no content on the wire: the transport serializes headers only, after
# the payload has been sized from the full body. Not asserted here
# because it is daemon-wide behaviour rather than anything specific to
# this route -- 40-http-conformance covers it at the socket level.

# --- 5. Already-compressed payload is not gzip-encoded. -----------
# curl offers gzip in Accept-Encoding here. deflate over a PNG buys
# nothing and routinely grows the body, so the server must skip it.
_curl --compressed "$HOST/flags/us.png"
_assert_status 200 "GET /flags/us.png (Accept-Encoding: gzip)"
ENC=$(_header content-encoding)
if [ -z "$ENC" ]; then
	_pass "PNG is served unencoded even when gzip is offered"
else
	_fail "/flags/us.png Content-Encoding" "expected none, got: $ENC"
fi

# --- 6. The "unknown" placeholder is reachable. -------------------
# CCountryFlags::GetFlag() falls back to flags/unknown.png ("??") for
# an empty or unrecognised code; the same artwork has to be available
# here or a frontend can't match the desktop.
_curl "$HOST/flags/unknown.png"
_assert_status 200 "GET /flags/unknown.png (the \"??\" placeholder)"
SIG=$(od -An -tx1 -N8 "$CURL_BODY_FILE" | tr -d ' \n')
if [ "$SIG" = "89504e470d0a1a0a" ]; then
	_pass "placeholder body starts with the PNG signature"
else
	_fail "/flags/unknown.png body" "expected PNG magic, got ${SIG:-<empty>}"
fi

# --- 7. Malformed codes are 404, not 500 / not a wrong icon. ------
# The art id is built by concatenating "flag_" with the code, so the
# charset check is load-bearing: without it a crafted code could name a
# non-flag entry in the shared icon table.
while read -r target label; do
	[ -z "$target" ] && continue
	_curl "$HOST$target"
	_assert_status 404 "$label"
done <<-'EOF'
	/flags/DE.png	uppercase code → 404
	/flags/d.png	one-letter code → 404
	/flags/deu.png	three-letter code → 404
	/flags/de.jpg	wrong extension → 404
	/flags/de	no extension → 404
	/flags/	empty code → 404
	/flags/d1.png	digit in code → 404
	/flags/zz.png	well-formed code with no artwork → 404
EOF

# Traversal attempts. `..` is rejected upstream by the shared
# LooksMalicious gate (400), and the exact-shape check would 404 them
# anyway — either way, no icon comes back.
for target in "/flags/../amule.png" "/flags/%2e%2e/amule.png" "/flags/amule.png"; do
	_curl "$HOST$target"
	case "$CURL_STATUS" in
		400|404) _pass "traversal $target is refused (HTTP $CURL_STATUS)" ;;
		*) _fail "traversal $target" "expected 400 or 404, got $CURL_STATUS" ;;
	esac
	SIG=$(od -An -tx1 -N8 "$CURL_BODY_FILE" | tr -d ' \n')
	if [ "$SIG" = "89504e470d0a1a0a" ]; then
		_fail "traversal $target body" "response leaked a PNG from the icon table"
	fi
done

# --- 8. Non-safe methods are 405. ---------------------------------
for method in POST PATCH DELETE; do
	_curl -X "$method" "$HOST/flags/de.png"
	_assert_status 405 "$method /flags/de.png → 405"
done

# --- 9. No auth required. -----------------------------------------
# Every assertion above ran without a token, so reaching this line at
# all proves it. Assert explicitly with a deliberately bogus bearer so
# a future auth gate on the route can't slip through unnoticed.
_curl -H "Authorization: Bearer not-a-real-token" "$HOST/flags/fr.png"
_assert_status 200 "GET /flags/fr.png with a bogus bearer is still served"

# --- 10. Density variants preserve validation and caching. --------
for scale in 2 3; do
    for code in us an unknown; do
        _curl "$HOST/flags/$code@${scale}x.png"
        _assert_status 200 "GET $code @${scale}x"
        case "$(_header content-type)" in
            image/png*) _pass "@${scale}x PNG Content-Type" ;;
            *) _fail "@${scale}x Content-Type" ;;
        esac
        DIMS=$(od -An -tu1 -j16 -N8 "$CURL_BODY_FILE" | xargs)
        EXPECTED="0 0 0 $((16*scale)) 0 0 0 $((12*scale))"
        if [ "$DIMS" = "$EXPECTED" ]; then _pass "@${scale}x dimensions"; else _fail "@${scale}x dimensions" "$DIMS"; fi
        DENSITY_ETAG=$(_header etag)
        if [ -n "$DENSITY_ETAG" ]; then
            _curl -H "If-None-Match: $DENSITY_ETAG" "$HOST/flags/$code@${scale}x.png"
            _assert_status 304 "@${scale}x matching ETag"
            if [ "$CURL_SIZE" = 0 ]; then _pass "density 304 empty"; else _fail "density 304 body"; fi
        else _fail "density ETag missing"; fi
        _curl -I "$HOST/flags/$code@${scale}x.png"
        _assert_status 200 "HEAD @${scale}x"
        if [ "$CURL_SIZE" = 0 ]; then _pass "density HEAD empty"; else _fail "density HEAD body"; fi
    done
    _curl -X POST "$HOST/flags/us@${scale}x.png"
    _assert_status 405 "POST density refused"
done
for target in us.svg us@1x.png us@4x.png us@2X.png us@2x@3x.png DE@2x.png zz@3x.png unknown@4x.png; do
    _curl "$HOST/flags/$target"
    _assert_status 404 "unsupported flag name $target"
done

# --- Summary. -----------------------------------------------------
_summary
