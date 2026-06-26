//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef LIBWEBCOMMON_ETAG_H
#define LIBWEBCOMMON_ETAG_H

#include <string>

// ETag computation + If-None-Match comparison for the REST API.
// Single source of truth for the digest-truncation rule — every
// binary uses the same algorithm so client caches stay valid
// across daemons.

namespace webcommon
{

// SHA-256 over `body_utf8`, truncated to the leading 8 bytes and
// rendered as 16 lowercase hex chars. 64 bits of digest gives a
// 1-in-2^64 collision probability across one connection's lifetime
// and the 16-char ETag stays under the IETF-recommended header
// budget. RFC 7232 §2.3 requires quotes around the header value;
// the caller wraps when assembling `ETag: "<hex>"`. Bare hex is
// returned so the same value feeds straight into IfNoneMatchHits.
std::string Etag(const std::string &body_utf8);

// RFC 7232 §3.2 conditional-GET match. `if_none_match` is the raw
// header value; `etag` is the bare-hex value returned by Etag().
// Returns true when the caller should swap a 200 + body response
// for a 304 Not Modified.
//
// Accepted client shapes (per RFC 7232 §2.3 + §3.2):
//  * `"<hex>"`        — strong validator, RFC-canonical form
//  * `W/"<hex>"`      — weak validator (same opaque payload)
//  * `<hex>`          — bare hex, tolerated for non-canonical clients
//  * `*`              — wildcard, matches any existing representation
//  * `"<a>", W/"<b>"` — comma-separated list, any-match wins
//
// Whitespace around list entries is stripped; match is case-
// sensitive on the hex payload (RFC §2.3.2 — opaque-string
// equality).
bool IfNoneMatchHits(const std::string &if_none_match, const std::string &etag);

} // namespace webcommon

#endif // LIBWEBCOMMON_ETAG_H
