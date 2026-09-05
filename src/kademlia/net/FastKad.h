//								-*- C++ -*-
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2026 eMule AI
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

#ifndef __KAD_FASTKAD_H__
#define __KAD_FASTKAD_H__

#include <cstddef>
#include <map>

#include "../../Types.h"

////////////////////////////////////////
namespace Kademlia
{
////////////////////////////////////////

// Derives the Kad request timeout from observed response times instead of a
// fixed constant.
//
// A fixed timeout is wrong in both directions: on a fast link it wastes seconds
// waiting for a node that was never going to answer, and on a slow or congested
// one it discards contacts that would have answered just after the deadline.
// This keeps a bounded window of the most recent per-address response times and
// estimates the ceiling as mean + 2 standard deviations (about the 95th
// percentile of a normal distribution), plus a small margin.
//
// Two deliberate biases keep a cold or lucky window from producing an
// aggressively short timeout:
//
//  - The mean and variance are always divided by the *full* window size, with
//    every unfilled slot counted as the default response time. A handful of
//    fast samples therefore cannot pull the estimate far below the default;
//    it takes a full window of fast samples to earn a short timeout.
//  - Entries younger than MIN_EVICTION_AGE_MS are never evicted to make room,
//    so a burst of new addresses cannot churn the whole window at once.
//
// All time values are in milliseconds and "now" is always passed in, which is
// what lets the estimator be tested without waiting on a real clock.
class CFastKad
{
public:
	// Number of addresses whose response time is remembered.
	static const size_t MAX_SAMPLES = 100;
	// Response time assumed for every unfilled slot in the window, and the
	// answer while the window is empty.
	static const uint32_t DEFAULT_RESPONSE_TIME_MS = 1000;
	// Absolute ceiling on the derived timeout. A node that has not answered
	// within this is treated as gone no matter how bad the samples look;
	// without a cap, one pathological sample would stall every search.
	static const uint32_t MAX_RESPONSE_TIME_MS = 3000;
	// Safety margin added to the estimate, for the jitter between the
	// estimator's clock and the actual send/receive path.
	static const uint32_t RESPONSE_TIME_MARGIN_MS = 100;
	// Samples younger than this are kept even when the window is full, so a
	// flood of new addresses cannot evict the recent history in one go.
	static const uint32_t MIN_EVICTION_AGE_MS = 5 * 60 * 1000;

	CFastKad();

	// Records that the node at `ip` answered in `responseTimeMs`. Ignored if
	// the window is full of samples too young to evict.
	void AddResponseTime(uint32_t ip, uint32_t responseTimeMs, uint64_t nowMs);

	// The current estimated ceiling, always within
	// [RESPONSE_TIME_MARGIN_MS, MAX_RESPONSE_TIME_MS].
	uint32_t GetEstMaxResponseTime() const { return m_estResponseTimeMs; }

	size_t GetSampleCount() const { return m_samples.size(); }

	// Drops every sample and returns the estimate to its cold-start value.
	void Clear();

private:
	void RecalculateResponseTime();

	struct sResponseTime
	{
		uint32_t m_responseTimeMs;
		uint64_t m_lastReferencedMs;
	};

	std::map<uint32_t, sResponseTime> m_samples;
	uint32_t m_estResponseTimeMs;
};

// The single estimator shared by every Kad search. Kad is driven from
// CamuleApp::OnCoreTimer on the main thread, like the rest of the Kad
// singletons reached through CKademlia.
extern CFastKad fastKad;

} // namespace Kademlia

#endif // __KAD_FASTKAD_H__
