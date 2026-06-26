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

#ifndef WEBAPI_TTL_CACHE_H
#define WEBAPI_TTL_CACHE_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace webapi
{

// Single-flight TTL cache for lazy-fetched endpoints
// (/logs/serverinfo, /stats/tree, /stats/graphs/{graph},
// /search/results). HTTP handlers drive their own EC fetches on
// demand, coalescing burst reads via a 1 s TTL.
//
// **Single-flight semantics.** Only one thread runs `fetch()` for a
// given stale-or-unset cache; concurrent callers park on a condvar
// while the inflight thread runs the EC roundtrip with the cache
// mutex DROPPED. Once the result is stored and the flag clears,
// every waiter observes the just-stored value and returns it without
// re-fetching.
//
// **Why drop the mutex around fetch?** A 30 s amuled stall on
// /stats/tree would otherwise park every concurrent reader of that
// endpoint for the stall's duration. Dropping the cache mutex around
// the EC call lets concurrent readers cooperate on the single
// inflight fetch without serialising on the slowest amuled response.
//
// **Lock ordering.** The fetcher lambda acquires `m_ec_mtx` while
// this cache's `m_mu` is NOT held. Still single-flight per endpoint
// because `m_inflight` gates concurrent fetches.
//
// The cached `T` must be copyable (returned by value so `m_mu` isn't
// held across JSON serialisation).
template <class T> class CTtlCache
{
public:
	using clock_t = std::chrono::steady_clock;

	// Returns a copy of the freshest value. If the cache is fresh,
	// returns immediately under a brief lock. If stale-or-unset:
	// one caller becomes the "inflight" worker (drops the lock,
	// runs fetch, re-takes the lock, stores, broadcasts);
	// concurrent callers wait on the condvar until inflight clears
	// and then read the stored value.
	template <class Fetcher> T GetOrFetch(std::chrono::milliseconds ttl, Fetcher fetch)
	{
		std::unique_lock<std::mutex> lk(m_mu);
		while (true) {
			const auto now = clock_t::now();
			const bool unset = (m_fetched_at == clock_t::time_point{});
			if (!unset && (now - m_fetched_at) <= ttl) {
				return m_value;
			}
			if (m_inflight) {
				// Another caller is doing the EC roundtrip. Wait
				// for them to finish, then re-check freshness in
				// case yet another fetch is needed (e.g. the
				// inflight result raced our TTL clamp because
				// fetch was slow).
				m_cv.wait(lk, [this] { return !m_inflight; });
				continue;
			}
			// We're the inflight worker. Claim the slot, drop the
			// lock, do the fetch unguarded so peers can park on
			// the condvar without our long EC call serialising
			// them on the cache mutex.
			m_inflight = true;
			lk.unlock();
			T fetched;
			try {
				fetched = fetch();
			} catch (...) {
				// Clear inflight + wake waiters even on exception
				// so we don't park them forever.
				{
					std::lock_guard<std::mutex> g(m_mu);
					m_inflight = false;
				}
				m_cv.notify_all();
				throw;
			}
			{
				std::lock_guard<std::mutex> g(m_mu);
				m_value = std::move(fetched);
				m_fetched_at = clock_t::now();
				m_inflight = false;
			}
			m_cv.notify_all();
			// Re-acquire to read the value under the lock so
			// the contract (returned T is a stable copy of the
			// just-stored snapshot) holds in the face of a racing
			// Invalidate.
			std::lock_guard<std::mutex> g(m_mu);
			return m_value;
		}
	}

	// Invalidate. Future GetOrFetch will trigger a fresh fetch
	// regardless of TTL. Used by mutations that touch an
	// endpoint's data (e.g. POST /search invalidating
	// /search/results).
	void Invalidate()
	{
		std::lock_guard<std::mutex> g(m_mu);
		m_fetched_at = clock_t::time_point{};
	}

private:
	mutable std::mutex m_mu;
	std::condition_variable m_cv;
	clock_t::time_point m_fetched_at{};
	T m_value{};
	bool m_inflight = false;
};

} // namespace webapi

#endif // WEBAPI_TTL_CACHE_H
