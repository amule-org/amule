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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef RATELIMITER_H
#define RATELIMITER_H

#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

/**
 * Per-IP sliding-window failure rate limiter.
 *
 * Tracks failed authentication attempts and locks the offending address out
 * for `lockout_seconds` once `threshold` failures land inside
 * `window_seconds`. A success resets that address's bucket.
 *
 * Lives in mulecommon because both authenticated front doors need it: the
 * amuleapi HTTP login and the External Connection password exchange. Nothing
 * here touches wx or any protocol, so it stays linkable from the daemon,
 * which does not link the webapi library.
 *
 * Storage: one bucket per address. Real deployments serve a small population
 * (a LAN operator, a handful of clients), so the map stays small even under
 * bot-scan load. No active GC; cold buckets are overwritten when the offender
 * returns, and process lifetime bounds the worst case.
 */
class CRateLimiter
{
public:
	struct Config
	{
		unsigned window_seconds = 60;
		unsigned threshold = 5;
		unsigned lockout_seconds = 300;
	};

	/// Clock injection. Default is std::time(nullptr); tests pass a
	/// controllable lambda to exercise the sliding-window logic in
	/// microseconds instead of sleeping through real seconds.
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

	/// Called BEFORE the credential compare. When `locked_out` is set the
	/// caller must refuse without touching the credential path, reporting
	/// `retry_after_seconds`.
	Decision Check(const std::string &ip);

	/// Called AFTER a failed credential compare. Records the failure and
	/// arms the lockout once the threshold is crossed.
	void NoteFailure(const std::string &ip);

	/// Called AFTER a successful credential compare. Drops the bucket so a
	/// legitimate user's next attempt is not accounted against an earlier
	/// streak of typos.
	void NoteSuccess(const std::string &ip);

	const Config &Cfg() const { return m_cfg; }

private:
	struct Bucket
	{
		// Sliding window of failure timestamps. A plain counter plus a
		// window start would implement a TUMBLING window, where an
		// attacker can spend threshold-1 failures at the end of one
		// window and threshold-1 more at the start of the next and never
		// trip the lockout. One timestamp per failure, expired
		// individually, closes that gap.
		std::deque<std::time_t> failures;
		std::time_t lockout_until = 0;
	};

	Config m_cfg;
	Clock m_clock;
	mutable std::mutex m_mu;
	std::map<std::string, Bucket> m_buckets;
};

#endif // RATELIMITER_H
