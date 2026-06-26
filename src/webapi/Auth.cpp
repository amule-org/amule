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

// ---------- CRateLimiter -----------------------------------------

CRateLimiter::Decision CRateLimiter::Check(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(m_mu);
	const std::time_t now = m_clock();
	auto it = m_buckets.find(ip);
	if (it == m_buckets.end())
		return Decision{};

	Bucket &b = it->second;
	if (b.lockout_until > now) {
		Decision d;
		d.locked_out = true;
		d.retry_after_seconds = b.lockout_until - now;
		return d;
	}
	// Lockout window expired — wipe the bucket so a stale lockout
	// can't accidentally fire on the next Check after a long quiet
	// period.
	if (b.lockout_until != 0 && b.lockout_until <= now) {
		b.lockout_until = 0;
		b.failures.clear();
	}
	// Mirror NoteFailure's per-stamp expiry so Check is self-
	// consistent. Otherwise stale stamps from a long-idle bucket
	// remain in failures until the next NoteFailure fires, and a
	// caller inspecting bucket size via a future debug surface would
	// see counts that include already-out-of-window failures.
	while (!b.failures.empty() && (now - b.failures.front()) > m_cfg.window_seconds) {
		b.failures.pop_front();
	}
	return Decision{};
}

void CRateLimiter::NoteFailure(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(m_mu);
	const std::time_t now = m_clock();
	Bucket &b = m_buckets[ip];

	// Sliding window: drop any failure stamp older than
	// `window_seconds`, then append now. Lockout fires when the live
	// stamp count crosses `threshold`. Previously the bucket reset
	// wholesale once `now - window_start > window_seconds`, which
	// implemented a TUMBLING window — an attacker could split
	// threshold-1 attempts across the two adjacent windows and
	// never trip lockout. Per-stamp expiry closes the gap.
	while (!b.failures.empty() && (now - b.failures.front()) > m_cfg.window_seconds) {
		b.failures.pop_front();
	}
	b.failures.push_back(now);

	if (b.failures.size() >= m_cfg.threshold) {
		b.lockout_until = now + m_cfg.lockout_seconds;
	}
}

void CRateLimiter::NoteSuccess(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(m_mu);
	m_buckets.erase(ip);
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

// ---------- ISO-8601 ---------------------------------------------

std::string FormatIso8601Utc(std::time_t t)
{
	std::tm out{};
#ifdef _WIN32
	gmtime_s(&out, &t);
#else
	gmtime_r(&t, &out);
#endif
	char buf[32];
	const int n = std::snprintf(buf,
		sizeof(buf),
		"%04d-%02d-%02dT%02d:%02d:%02dZ",
		out.tm_year + 1900,
		out.tm_mon + 1,
		out.tm_mday,
		out.tm_hour,
		out.tm_min,
		out.tm_sec);
	// snprintf's return is the bytes that *would* have been written.
	// Our format produces exactly 20 chars + NUL, so anything else
	// is a libc bug — return whatever we got rather than crashing.
	if (n < 0)
		return std::string();
	return std::string(buf, std::min<size_t>(n, sizeof(buf) - 1));
}

} // namespace webapi
