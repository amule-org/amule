//
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

#include "FastKad.h"

#include <algorithm>
#include <cmath>

namespace Kademlia
{

CFastKad fastKad;

CFastKad::CFastKad()
: m_estResponseTimeMs(DEFAULT_RESPONSE_TIME_MS)
{
}

void CFastKad::Clear()
{
	m_samples.clear();
	m_estResponseTimeMs = DEFAULT_RESPONSE_TIME_MS;
}

void CFastKad::AddResponseTime(uint32_t ip, uint32_t responseTimeMs, uint64_t nowMs)
{
	std::map<uint32_t, sResponseTime>::iterator it = m_samples.find(ip);
	if (it == m_samples.end()) {
		if (m_samples.size() >= MAX_SAMPLES) {
			// Evict the least recently referenced sample, but only if
			// it is old enough that dropping it will not make the
			// estimate jump around.
			uint64_t oldestAge = 0;
			std::map<uint32_t, sResponseTime>::iterator oldest = m_samples.end();
			for (std::map<uint32_t, sResponseTime>::iterator candidate = m_samples.begin();
				candidate != m_samples.end();
				++candidate) {
				const uint64_t age = nowMs - candidate->second.m_lastReferencedMs;
				if (age >= oldestAge && age > MIN_EVICTION_AGE_MS) {
					oldestAge = age;
					oldest = candidate;
				}
			}
			if (oldest == m_samples.end()) {
				// Every sample is recent. Discarding this
				// observation is the right call: the window
				// already holds a fresh picture of the network.
				return;
			}
			m_samples.erase(oldest);
		}
		sResponseTime added = { responseTimeMs, nowMs };
		m_samples[ip] = added;
	} else {
		it->second.m_responseTimeMs = responseTimeMs;
		it->second.m_lastReferencedMs = nowMs;
	}

	RecalculateResponseTime();
}

void CFastKad::RecalculateResponseTime()
{
	// Unfilled slots count as DEFAULT_RESPONSE_TIME_MS, and both moments are
	// divided by the full window size rather than by the sample count. That
	// is what makes the cold start safe -- with zero or one sample the
	// estimate stays at the default instead of collapsing, and there is no
	// division by a zero (or by count - 1 == 0) sample count anywhere.
	const double windowSize = (double)MAX_SAMPLES;
	const double missing =
		(m_samples.size() < MAX_SAMPLES) ? (windowSize - (double)m_samples.size()) : 0.0;

	double sum = 0.0;
	for (std::map<uint32_t, sResponseTime>::const_iterator it = m_samples.begin(); it != m_samples.end();
		++it) {
		sum += (double)it->second.m_responseTimeMs;
	}
	sum += missing * (double)DEFAULT_RESPONSE_TIME_MS;
	const double mean = sum / windowSize;

	double varianceSum = 0.0;
	for (std::map<uint32_t, sResponseTime>::const_iterator it = m_samples.begin(); it != m_samples.end();
		++it) {
		const double deviation = (double)it->second.m_responseTimeMs - mean;
		varianceSum += deviation * deviation;
	}
	varianceSum += missing * ((double)DEFAULT_RESPONSE_TIME_MS - mean) *
		       ((double)DEFAULT_RESPONSE_TIME_MS - mean);
	const double variance = varianceSum / (windowSize - 1.0);

	// mean + 2 * standard deviation is roughly the 95th percentile of a
	// normal distribution, plus a fixed margin, and never above the cap.
	const double estimate = mean + 2.0 * std::sqrt(variance) + (double)RESPONSE_TIME_MARGIN_MS;
	m_estResponseTimeMs =
		(estimate >= (double)MAX_RESPONSE_TIME_MS) ? MAX_RESPONSE_TIME_MS : (uint32_t)estimate;
	if (m_estResponseTimeMs < RESPONSE_TIME_MARGIN_MS) {
		m_estResponseTimeMs = RESPONSE_TIME_MARGIN_MS;
	}
}

} // namespace Kademlia
