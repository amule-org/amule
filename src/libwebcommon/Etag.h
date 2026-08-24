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

// Suffix distinguishing the gzipped representation of a body from the identity
// one. A strong validator identifies ONE representation, so the same ETag must
// not describe both codings of a resource -- a cache holding the gzip form and
// revalidating for a client that cannot accept it would otherwise be told its
// copy is current. The hash is taken before compression, so the coding is
// appended by whoever selects the representation, and the conditional-GET
// comparison runs against that same value -- matching either coding would
// defeat the point, telling a client holding one representation that its copy
// of the other is current.
extern const char kGzipEtagSuffix[];

// Returns `etag` naming the gzipped representation when `coded`, and the
// identity one when not. Accepts either the bare-hex or the RFC-quoted form
// and preserves which one it was given.
//
// Idempotent, and defined by the representation rather than by the edit: the
// caller says which coding the value must name, not whether to add or remove
// anything. That is what lets the transport reconcile the dispatcher's
// prediction against what compression actually did -- when deflate fails after
// the suffix was already stamped, the same call takes it back off, and when
// the prediction held it changes nothing.
std::string WithCodingSuffix(const std::string &etag, bool coded);

} // namespace webcommon

#endif // LIBWEBCOMMON_ETAG_H
