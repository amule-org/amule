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

#include "Auth.h"

#include "HeaderParse.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

// strncasecmp on POSIX is declared in <strings.h>. Glibc also exposes it via <string.h>, but musl
// and the BSDs do not -- be explicit so the build does not depend on the implicit include. Mirrors
// the shim libwebcommon/HeaderParse.cpp already uses.
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

namespace webapi
{

// ---------- CRevocationSet ---------------------------------------

void CRevocationSet::Revoke(const std::string &jti, std::time_t exp)
{
	std::lock_guard<std::mutex> lock(m_mu);
	m_revoked[jti] = exp;
	GcExpired();
}

bool CRevocationSet::IsRevoked(const std::string &jti) const
{
	std::lock_guard<std::mutex> lock(m_mu);
	auto it = m_revoked.find(jti);
	if (it == m_revoked.end())
		return false;
	// Lazy GC: if the entry has already expired, drop it. Saves a tick of memory and stops
	// stale entries accumulating for tokens nobody will ever present again.
	if (it->second <= std::time(nullptr)) {
		m_revoked.erase(it);
		return false;
	}
	return true;
}

std::size_t CRevocationSet::Size() const
{
	std::lock_guard<std::mutex> lock(m_mu);
	return m_revoked.size();
}

void CRevocationSet::GcExpired() const
{
	// O(n) sweep over the revoked map, fired from every Revoke() and every Contains() check.
	// Fine at amuleapi's expected scale -- a single operator, a handful of sessions per day --
	// so the map stays in the low hundreds even under aggressive re-login.
	const std::time_t now = std::time(nullptr);
	for (auto it = m_revoked.begin(); it != m_revoked.end();) {
		if (it->second <= now) {
			it = m_revoked.erase(it);
		} else {
			++it;
		}
	}
}

// ---------- Header extraction ------------------------------------

std::string ExtractBearerToken(const std::string &authorization_header)
{
	// `Authorization: Bearer <jwt>` per RFC 6750 2.1. The scheme name is
	// case-insensitive; the token is the bare base64url triplet CJwt emits.
	const char *prefix = "Bearer ";
	const size_t plen = std::strlen(prefix);
	if (authorization_header.size() <= plen)
		return std::string();
	if (strncasecmp(authorization_header.c_str(), prefix, plen) != 0) {
		return std::string();
	}
	// Trim leading OWS after the scheme name (some clients add extra
	// spaces; RFC 7230 §3.2.3 allows them).
	size_t i = plen;
	while (i < authorization_header.size() &&
		(authorization_header[i] == ' ' || authorization_header[i] == '\t')) {
		++i;
	}
	if (i >= authorization_header.size())
		return std::string();
	return authorization_header.substr(i);
}

std::string ExtractCookieValue(const std::string &cookie_header, const std::string &cookie_name)
{
	// Delegate to libwebcommon's helper so the parsing rules (case-insensitive
	// name match, `;` separators, OWS trimming) stay in one place.
	const auto v =
		webcommon::FindCookieValue(cookie_header.c_str(), cookie_header.size(), cookie_name.c_str());
	if (!v.first || v.second == 0)
		return std::string();
	return std::string(v.first, v.second);
}

namespace
{

bool EqualsNoCase(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
}

// "scheme://host[:port]" -> "host[:port]"; empty for "null" or anything else.
std::string OriginAuthority(const std::string &origin)
{
	const std::string::size_type sep = origin.find("://");
	return sep == std::string::npos ? std::string() : origin.substr(sep + 3);
}

// A proxy may list several hosts; the first is the one the client asked for.
std::string FirstListEntry(const std::string &list)
{
	const std::string first = list.substr(0, list.find(','));
	const std::string::size_type b = first.find_first_not_of(" \t");
	if (b == std::string::npos) {
		return std::string();
	}
	return first.substr(b, first.find_last_not_of(" \t") - b + 1);
}

} // namespace

bool IsCrossOriginBrowserWrite(const std::string &method,
	const std::string &sec_fetch_site,
	const std::string &origin,
	bool origin_allowlisted,
	const std::string &host,
	const std::string &forwarded_host)
{
	if (method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE") {
		return false;
	}
	if (origin_allowlisted) {
		return false;
	}
	if (!sec_fetch_site.empty()) {
		// "none" is a request the user started, such as a bookmark.
		return !EqualsNoCase(sec_fetch_site, "same-origin") && !EqualsNoCase(sec_fetch_site, "none");
	}
	if (origin.empty()) {
		return false;
	}
	const std::string authority = OriginAuthority(origin);
	if (authority.empty()) {
		return true;
	}
	const std::string forwarded = FirstListEntry(forwarded_host);
	return !EqualsNoCase(authority, host) && (forwarded.empty() || !EqualsNoCase(authority, forwarded));
}

} // namespace webapi
