//								-*- C++ -*-
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

#ifndef AMULE_PUBLISHMETRICS_H
#define AMULE_PUBLISHMETRICS_H

#include "LookupTrace.h"
#include <algorithm>

namespace Kademlia
{
// Main-thread diagnostics, called only while the Kademlia Search category is enabled.
class CPublishMetrics
{
public:
	static CPublishMetrics &Instance()
	{
		static CPublishMetrics metrics;
		return metrics;
	}
	void Attempt(bool started)
	{
		++attempts;
		startFailures += !started;
		++changes;
	}
	void Start()
	{
		++starts;
		++changes;
	}
	void Queued()
	{
		++queued;
		++changes;
	}
	static uint64_t Acknowledged(const CLookupTrace &trace)
	{
		uint64_t count = 0;
		uint32_t previous = 0;
		bool havePrevious = false;
		for (const auto &peer : trace.Peers()) {
			if (peer.second.itemReplies && (!havePrevious || previous != peer.first.first)) {
				++count;
				previous = peer.first.first;
				havePrevious = true;
			}
		}
		return count;
	}
	void Finish(const CLookupTrace &trace, bool completed, uint64_t elapsed)
	{
		++changes;
		acknowledged += Acknowledged(trace);
		if (!completed) {
			++cancelled;
			return;
		}
		++finished;
		zeroAcknowledgements += Acknowledged(trace) == 0;
		durationMs += elapsed;
		maxDurationMs = std::max(maxDurationMs, elapsed);
	}
	static constexpr size_t MaxRateSamples = 65536;
	void LookupQueued(uint32_t ip, uint64_t now)
	{
		Expire(now);
		if (requests.size() == MaxRateSamples) {
			++omittedRateSamples;
			++changes;
			return;
		}
		requests.emplace_back(now, ip);
		const auto count = ++counts[ip];
		if (count > maxRequestsPerIP60s) {
			maxRequestsPerIP60s = count;
			++changes;
		}
	}
	bool ReportDue(uint64_t now)
	{
		Expire(now);
		if (!reportStarted) {
			reportStarted = true;
			lastReport = now;
			return false;
		}
		if (now - lastReport < 60000) {
			return false;
		}
		lastReport = now;
		if (reportedChanges == changes) {
			return false;
		}
		reportedChanges = changes;
		return true;
	}
	uint64_t attempts = 0, startFailures = 0, starts = 0;
	uint64_t queued = 0, acknowledged = 0, finished = 0, zeroAcknowledgements = 0, cancelled = 0;
	uint64_t durationMs = 0, maxDurationMs = 0, maxRequestsPerIP60s = 0, omittedRateSamples = 0;

private:
	void Expire(uint64_t now)
	{
		while (!requests.empty() && now - requests.front().first >= 60000) {
			auto it = counts.find(requests.front().second);
			if (--it->second == 0) {
				counts.erase(it);
			}
			requests.pop_front();
		}
	}
	bool reportStarted = false;
	uint64_t lastReport = 0, changes = 0, reportedChanges = 0;
	std::deque<std::pair<uint64_t, uint32_t>> requests;
	std::map<uint32_t, uint64_t> counts;
};
} // namespace Kademlia
#endif
