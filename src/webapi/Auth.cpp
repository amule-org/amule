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

// strncasecmp on POSIX is declared in <strings.h>. Glibc also exposes
// it via <string.h>, but musl and the BSDs do not — be explicit so
// the build doesn't depend on the implicit include. Mirror the shim
// libwebcommon/HeaderParse.cpp already uses.
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
	// Lazy GC: if the entry has already expired, drop it. Saves a
	// tick of memory and prevents stale entries from accumulating
	// for tokens nobody will ever present again.
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
	// O(n) sweep over the revoked map, fired from every Revoke() and
	// every Contains() check. Fine at amuleapi's expected scale (a
	// single operator, a handful of admin/guest sessions per day);
	// the map stays in the low hundreds even under aggressive
	// re-login. If multi-tenant deployments ever raise the revoked
	// population into the thousands, swap this for a min-heap keyed
	// by `exp` so the GC pops a constant prefix per call instead of
	// walking the whole structure.
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
	// `Authorization: Bearer <jwt>` per RFC 6750 §2.1. Scheme name is
	// case-insensitive; the token itself is the bare base64url
	// triplet our own CJwt emits.
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
	// Delegate to libwebcommon's pointer-arithmetic helper so the
	// parsing rules (case-insensitive name match, `;` separators,
	// OWS trimming) stay in one place.
	const auto v =
		webcommon::FindCookieValue(cookie_header.c_str(), cookie_header.size(), cookie_name.c_str());
	if (!v.first || v.second == 0)
		return std::string();
	return std::string(v.first, v.second);
}

} // namespace webapi
