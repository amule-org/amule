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

// Per-IP sliding-window login rate limiter (). Tracks
// failed `/auth/login` attempts, locks the offending IP out for
// `lockout_seconds` once `threshold` failures land inside
// `window_seconds`. A successful login resets the offender's bucket.
//
// Storage: std::unordered_map<ip_string, Bucket>. Real-world
// amuleapi deployments serve a small population (LAN ops, single
// operator), so the map stays under a few hundred entries even
// under bot-scan load. No active GC; cold buckets get overwritten
// when the offender comes back, and the daemon's process lifetime
// bounds the worst case.
class CRateLimiter
{
public:
	struct Config
	{
		unsigned window_seconds = 60;
		unsigned threshold = 5;
		unsigned lockout_seconds = 300;
	};

	// Clock injection. Default is std::time(nullptr); tests pass a
	// controllable lambda so AuthTest can exercise the sliding-
	// window logic in microseconds instead of spending five+
	// real-time seconds sleeping between failures.
	using Clock = std::function<std::time_t()>;

	explicit CRateLimiter(Config cfg, Clock clock = nullptr)
	: m_cfg(cfg)
	, m_clock(clock ? std::move(clock) : [] { return std::time(nullptr); })
	{
	}

	struct Decision
	{
		bool locked_out = false;
		std::time_t retry_after_seconds = 0;
	};

	// Called BEFORE the password compare. If `locked_out` is true,
	// the caller emits 429 with `Retry-After: <retry_after_seconds>`
	// and never touches the credential path.
	Decision Check(const std::string &ip);

	// Called AFTER a failed credential compare. Updates the bucket
	// and possibly arms the lockout for next time.
	void NoteFailure(const std::string &ip);

	// Called AFTER a successful credential compare. Drops the
	// bucket so the user's next login isn't accounted against the
	// previous failure streak.
	void NoteSuccess(const std::string &ip);

	const Config &Cfg() const { return m_cfg; }

private:
	struct Bucket
	{
		// Sliding window of failure timestamps. The legacy `unsigned
		// failure_count + std::time_t window_start` shape implemented
		// a TUMBLING window (the count reset wholesale when the
		// window expired) — an attacker could burn threshold-1
		// failures in the last second of window N, threshold-1 more
		// in the first second of window N+1, and never trip lockout.
		// We now keep one timestamp per recorded failure (older than
		// `window_seconds` evicted on each NoteFailure) so the
		// trip happens whenever the live count crosses threshold,
		// regardless of how the failures distribute across windows.
		std::deque<std::time_t> failures;
		std::time_t lockout_until = 0;
	};

	Config m_cfg;
	Clock m_clock;
	mutable std::mutex m_mu;
	std::map<std::string, Bucket> m_buckets;
};

// HTTP `Authorization: Bearer <jwt>` extractor. Returns the empty
// string if the header is absent, doesn't start with `Bearer `, or
// has no value past the space. Case-insensitive scheme compare
// per RFC 6750 §2.1.
std::string ExtractBearerToken(const std::string &authorization_header);

// Extracts `<cookie_name>=<value>` from a Cookie header. Returns the
// empty string on miss.
std::string ExtractCookieValue(const std::string &cookie_header, const std::string &cookie_name);

// ISO-8601 / RFC 3339 in UTC: "2026-06-19T11:00:00Z". 20-char fixed
// length; clients can `Date.parse(...)` it.
std::string FormatIso8601Utc(std::time_t t);

} // namespace webapi

#endif // WEBAPI_AUTH_H
