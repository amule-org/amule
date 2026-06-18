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
#include <mutex>
#include <utility>


namespace webapi {


// Single-flight TTL cache for lazy-fetched endpoints (Phase 4g —
// /logs/serverinfo, /stats/tree, /stats/graphs/{graph},
// /search/results). The refresher's per-tick loop dropped these
// endpoints; the HTTP handlers now drive their own EC fetches on
// demand, coalescing burst reads via a short TTL (default 1 s — per
// operator preference: long enough to absorb dashboard tile bursts,
// short enough that data stays alive).
//
// **Single-flight semantics.** `GetOrFetch` holds `m_mu` for the
// entire check-fetch-store sequence. Concurrent HTTP threads racing
// the same lazy endpoint serialize on this mutex; the second waiter
// reads the cached value that the first thread just wrote (no
// duplicate EC roundtrip).
//
// **Lock ordering.** Callers acquire endpoint cache `m_mu` first,
// then take `m_ec_mtx` inside the fetcher lambda. Every other lazy
// endpoint uses the same ordering, so deadlock is impossible.
//
// The cached `T` must be copyable (we return by value to avoid
// holding `m_mu` across the JSON serialization).
template <class T>
class CTtlCache {
public:
	using clock_t = std::chrono::steady_clock;

	// Call `fetch()` and store the result under `m_mu` iff the
	// cached value is older than `ttl` (or unset). Returns a copy
	// of the freshest value — either the just-fetched one, or the
	// still-fresh cached one. `fetch` runs WITH `m_mu` held; it
	// must NOT call back into the same CTtlCache or it'll deadlock.
	template <class Fetcher>
	T GetOrFetch(std::chrono::milliseconds ttl, Fetcher fetch)
	{
		std::lock_guard<std::mutex> g(m_mu);
		const auto now = clock_t::now();
		const bool unset = (m_fetched_at == clock_t::time_point{});
		if (!unset && (now - m_fetched_at) <= ttl) {
			return m_value;
		}
		// Stale or unset — fetch under m_mu so a second waiter sees
		// the just-stored value.
		m_value = fetch();
		m_fetched_at = clock_t::now();
		return m_value;
	}

	// Invalidate. Future GetOrFetch will trigger a fresh fetch
	// regardless of TTL. Used by Phase 5+ mutations that touch an
	// endpoint's data (e.g. POST /search invalidating
	// /search/results).
	void Invalidate()
	{
		std::lock_guard<std::mutex> g(m_mu);
		m_fetched_at = clock_t::time_point{};
	}

private:
	mutable std::mutex      m_mu;
	clock_t::time_point     m_fetched_at{};
	T                       m_value{};
};


}  // namespace webapi

#endif // WEBAPI_TTL_CACHE_H
