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

#ifndef LIBWEBCOMMON_HEADERPARSE_H
#define LIBWEBCOMMON_HEADERPARSE_H

#include <cstddef>
#include <utility>

// Line-anchored HTTP header + Cookie helpers. Pure pointer-arithmetic
// over a CRLF-terminated header block — no copies, no allocations,
// no dependency on the HTTP server. Line-boundary anchoring (defeats
// `X-Foo:`-in-value header-injection) and the OWS trimming rules
// sit in one place.

namespace webcommon
{

// Walk an HTTP header block looking for `name:` at the start of a
// line (case-insensitive). Returns a non-owning view of the value,
// past the colon and any leading OWS, up to but not including the
// line's CRLF; returns {nullptr, 0} on miss. `block` is expected to
// start at the HTTP-version line (e.g. "GET / HTTP/1.1\r\n"); that
// line is skipped on the first iteration. Line-boundary anchoring
// defeats header-injection via a value that happens to contain a
// literal "X-Header:" — a strstr-based scan would have matched
// anywhere.
std::pair<const char *, std::size_t> FindHttpHeaderValue(const char *block, const char *name);

// Extract `cookie_name=value` from a Cookie-header value (already found
// via FindHttpHeaderValue — pass the view it returned). Returns
// {nullptr, 0} on miss. The returned view spans from past the `=` up
// to the next `;` or `cookies_len`, whichever comes first.
std::pair<const char *, std::size_t> FindCookieValue(
	const char *cookies, std::size_t cookies_len, const char *cookie_name);

} // namespace webcommon

#endif // LIBWEBCOMMON_HEADERPARSE_H
