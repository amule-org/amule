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


// ETag computation + If-None-Match comparison for the REST API. Pulled
// out so the same byte-for-byte algorithm runs in any caller — the wire
// contract (clients caching by ETag) breaks the moment the digest
// truncation rule diverges between binaries.

namespace webcommon {


// SHA-256 over `body_utf8`, truncated to the leading 8 bytes and
// rendered as 16 lowercase hex chars. The truncation is deliberate:
// 64 bits of digest gives a 1-in-2^64 collision probability across the
// life of a single connection — comfortably below the "client sees the
// wrong cached body" threshold — and a 16-char ETag stays well under
// the IETF-recommended HTTP header size budget. The caller is expected
// to wrap the return value in quotes when assembling the
// `ETag: "<hex>"` response header (RFC 7232 §2.3 requires the
// quotes; the helper returns the bare hex so the same value can be
// fed into IfNoneMatchHits without quote-stripping).
std::string Etag(const std::string &body_utf8);


// RFC 7232 §3.2 conditional-GET match. `if_none_match` is the raw
// header value as sent by the client. `etag` is the bare-hex value
// returned by Etag(). Returns true when the caller should swap a
// 200 + body response for a 304 Not Modified.
//
// Accepted client shapes (per RFC 7232 §2.3 and §3.2):
//   * `"<hex>"`         — strong validator, RFC-canonical form
//   * `W/"<hex>"`       — weak validator (same opaque payload as strong)
//   * `<hex>`           — bare hex, tolerated for non-canonical clients
//   * `*`               — wildcard, matches any existing representation
//   * `"<a>", W/"<b>"`  — comma-separated list, any-match wins
//
// Whitespace around list entries is stripped. The match itself is
// case-sensitive on the hex payload (RFC §2.3.2 — opaque-string
// equality). Phase 7 fixed the bare-vs-quoted mismatch noted in the
// Phase 1 retrospective.
bool IfNoneMatchHits(const std::string &if_none_match,
                     const std::string &etag);


}  // namespace webcommon

#endif // LIBWEBCOMMON_ETAG_H
