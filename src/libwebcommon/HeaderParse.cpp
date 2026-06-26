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

#include "HeaderParse.h"

#include <cstring>

// strncasecmp on POSIX is declared in <strings.h>, but most libc
// implementations also expose it via <string.h> + <cstring>. Be
// explicit so the build doesn't depend on the implicit include.
#ifdef _WIN32
#include <string.h>
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

namespace webcommon
{

std::pair<const char *, std::size_t> FindHttpHeaderValue(const char *block, const char *name)
{
	if (!block || !name)
		return { nullptr, 0 };
	const std::size_t name_len = std::strlen(name);
	const char *line = block;
	// `block` starts with the HTTP-version line of the request
	// ("HTTP/1.1\r\n..."), so the first iteration walks past it to
	// land on the first header.
	while (*line) {
		const char *eol = std::strstr(line, "\r\n");
		if (!eol)
			break;
		// End-of-headers marker: blank line.
		if (eol == line)
			break;
		// Move to the line *after* the current one for the next
		// iteration before doing the match, so an early `continue`
		// can't loop forever.
		const char *next = eol + 2;
		// Skip the version line on the first iteration: it doesn't
		// start with `name:` so the strncasecmp+colon check below
		// naturally rejects it.
		if (line + name_len < eol && strncasecmp(line, name, name_len) == 0 &&
			line[name_len] == ':') {
			const char *value = line + name_len + 1;
			// Strip leading OWS per RFC 7230 §3.2.
			while (*value == ' ' || *value == '\t')
				++value;
			// Strip trailing OWS likewise. Both sides may be
			// present per the spec, though no real client we've
			// ever seen sends trailing whitespace.
			const char *value_end = eol;
			while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
				--value_end;
			}
			return { value, static_cast<std::size_t>(value_end - value) };
		}
		line = next;
	}
	return { nullptr, 0 };
}

std::pair<const char *, std::size_t> FindCookieValue(
	const char *cookies, std::size_t cookies_len, const char *cookie_name)
{
	if (!cookies || !cookie_name)
		return { nullptr, 0 };
	const std::size_t name_len = std::strlen(cookie_name);
	const char *end = cookies + cookies_len;
	const char *p = cookies;
	while (p < end) {
		// Skip leading OWS / separators.
		while (p < end && (*p == ' ' || *p == '\t' || *p == ';'))
			++p;
		if (p + name_len + 1 > end)
			break;
		if (strncasecmp(p, cookie_name, name_len) == 0 && p[name_len] == '=') {
			const char *value = p + name_len + 1;
			const char *value_end = value;
			while (value_end < end && *value_end != ';')
				++value_end;
			// RFC 6265 §5.2 permits OWS (SP / HTAB) on either side
			// of `=` and between the value and the trailing `;`.
			// Strip both ends so a header like `Cookie: foo= bar `
			// matches a token-equal of `bar` rather than ` bar `.
			while (value < value_end && (*value == ' ' || *value == '\t'))
				++value;
			while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
				--value_end;
			}
			return { value, static_cast<std::size_t>(value_end - value) };
		}
		// Skip this cookie pair (up to next ';').
		while (p < end && *p != ';')
			++p;
	}
	return { nullptr, 0 };
}

} // namespace webcommon
