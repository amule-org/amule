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

#ifndef WEBAPI_AUTH_H
#define WEBAPI_AUTH_H

#include <deque>

#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <common/RateLimiter.h>

// State containers + helpers for the /auth/* surface. Live on the
// amuleapi process side (not in libwebcommon) because they're stateful
// and amuleweb has no use for them.
//
// Thread-safety model: today, every caller runs on the Boost.Asio I/O
// thread (single io_context, single std::thread). The std::mutex in
// each container is forward-compat insurance — the SSE channel adds
// a heartbeat timer that fires on the same I/O thread, so the mutex
// never contends in v0.1 — but a future worker-pool model gets
// correctness for free.

namespace webapi
{

// Server-side bearer-token revocation list. JWTs are stateless by
// design; for /auth/logout to actually invalidate a token, the server
// has to remember "this jti is dead until the JWT's exp".
//
// Memory cost: one entry per logged-out-but-still-unexpired token.
// `jti` is 22 base64url chars (~24 bytes once the std::string SSO
// boundary kicks in) + the exp timestamp + map overhead — call it
// ~64 bytes per revoked token. Bounded by max-concurrent-users ×
// 24 h (the JWT lifetime).
//
// GC: lazy on Revoke() — sweeps entries whose exp has already
// passed. Cheap (~O(log n) lookup per sweep) and amortizes the work
// across calls instead of needing a periodic timer.
class CRevocationSet
{
public:
	void Revoke(const std::string &jti, std::time_t exp);
	bool IsRevoked(const std::string &jti) const;

	// Test-visible inspection. Not exposed via the API — the
	// revocation set is operator-internal.
	std::size_t Size() const;

private:
	void GcExpired() const;

	mutable std::mutex m_mu;
	mutable std::map<std::string, std::time_t> m_revoked;
};

// The per-IP failure rate limiter now lives in mulecommon, shared with the
// External Connection password exchange, which needs the same protection but
// cannot link the webapi library. Re-exported here so existing
// `webapi::CRateLimiter` uses keep resolving.
using ::CRateLimiter;

// HTTP `Authorization: Bearer <jwt>` extractor. Returns the empty
// string if the header is absent, doesn't start with `Bearer `, or
// has no value past the space. Case-insensitive scheme compare
// per RFC 6750 §2.1.
std::string ExtractBearerToken(const std::string &authorization_header);

// Extracts `<cookie_name>=<value>` from a Cookie header. Returns the
// empty string on miss.
std::string ExtractCookieValue(const std::string &cookie_header, const std::string &cookie_name);

} // namespace webapi

#endif // WEBAPI_AUTH_H
