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

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <deque>
#include <map>

namespace Kademlia
{
// Main-thread diagnostics only. No scheduling decisions or persisted freshness use these values.
class CSourcePublishTrace
{
public:
	static constexpr size_t MaxDestinations = 128;
	void Start(uint64_t now)
	{
		started = now;
		active = true;
	}
	void Queued(uint32_t ip, uint16_t port) noexcept
	{
		++queued;
		try {
			if (destinations.count({ ip, port }) || destinations.size() < MaxDestinations) {
				destinations.emplace(std::make_pair(ip, port), false);
				return;
			}
		} catch (...) {
			// Sampling failure must not change the production store's control flow.
		}
		++omitted;
	}
	bool Acknowledge(uint32_t ip, uint16_t port)
	{
		auto it = destinations.find({ ip, port });
		if (it == destinations.end() || it->second) {
			return false;
		}
		// An accepted response must come from an endpoint we actually contacted.
		// Multiple ports on the same IP still represent one storage destination.
		bool alreadyAcknowledged = false;
		for (const auto &destination : destinations) {
			alreadyAcknowledged |= destination.first.first == ip && destination.second;
		}
		it->second = true;
		if (alreadyAcknowledged) {
			return false;
		}
		++acknowledged;
		return true;
	}
	uint64_t started = 0;
	uint64_t queued = 0;
	uint64_t acknowledged = 0;
	uint64_t omitted = 0;
	bool active = false;
	bool completed = false;
	bool cancelled = false;
	bool deadline = false;

private:
	std::map<std::pair<uint32_t, uint16_t>, bool> destinations;
};

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
		if (!started) {
			++startFailures;
		}
	}
	void Start()
	{
		++starts;
		++inFlight;
	}
	void Finish(CSourcePublishTrace &trace, uint64_t now)
	{
		if (!trace.active) {
			return;
		}
		trace.active = false;
		--inFlight;
		if (!trace.completed || trace.cancelled) {
			trace.cancelled = true;
			++cancelled;
			return;
		}
		++finished;
		zeroAcknowledgements += trace.acknowledged == 0;
		deadlines += trace.deadline;
		const auto elapsed = now - trace.started;
		durationMs += elapsed;
		maxDurationMs = std::max(maxDurationMs, elapsed);
	}
	bool ReportDue(uint64_t now)
	{
		if (!reportStarted) {
			reportStarted = true;
			lastReport = now;
			return false;
		}
		if (now - lastReport < 60000) {
			return false;
		}
		lastReport = now;
		return true;
	}
	uint64_t attempts = 0, startFailures = 0, starts = 0, inFlight = 0;
	uint64_t queued = 0, acknowledged = 0, omittedDestinations = 0;
	uint64_t finished = 0, zeroAcknowledgements = 0, deadlines = 0, cancelled = 0;
	uint64_t durationMs = 0, maxDurationMs = 0;

private:
	bool reportStarted = false;
	uint64_t lastReport = 0;
};
// The UDP throttler sends on a worker thread; snapshots never expose its containers.
class CPublishTransportMetrics
{
public:
	struct Snapshot
	{
		uint64_t sourcePackets = 0, sourceBytes = 0, failedSourceSends = 0;
		size_t rateSamples = 0;
		uint64_t maxRequestsPerIP60s = 0, omittedRateSamples = 0;
	};
	static CPublishTransportMetrics &Instance()
	{
		static CPublishTransportMetrics metrics;
		return metrics;
	}
	void SourceFinished(bool success, uint64_t bytes)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (success) {
			++stats.sourcePackets;
			stats.sourceBytes += bytes;
		} else {
			++stats.failedSourceSends;
		}
	}
	// Called only after a successful socket send, for ALL KADEMLIA2_REQ packets.
	// Bounds memory and exposes any lost samples; the peak is then a lower bound.
	static constexpr size_t MaxRateSamples = 65536;
	void LookupSent(uint32_t ip, uint64_t now)
	{
		std::lock_guard<std::mutex> lock(mutex);
		now = std::max(now, lastTick);
		Expire(now);
		if (requests.size() == MaxRateSamples) {
			++stats.omittedRateSamples;
			return;
		}
		try {
			auto entry = counts.try_emplace(ip, 0);
			try {
				requests.emplace_back(now, ip);
			} catch (...) {
				if (entry.second) {
					counts.erase(entry.first);
				}
				throw;
			}
			const auto count = ++entry.first->second;
			stats.maxRequestsPerIP60s = std::max(stats.maxRequestsPerIP60s, count);
		} catch (...) {
			++stats.omittedRateSamples;
		}
	}

	Snapshot GetSnapshot(uint64_t now)
	{
		std::lock_guard<std::mutex> lock(mutex);
		Expire(std::max(now, lastTick));
		auto snapshot = stats;
		snapshot.rateSamples = requests.size();
		return snapshot;
	}

private:
	void Expire(uint64_t now)
	{
		lastTick = now;
		while (!requests.empty() && now - requests.front().first >= 60000) {
			const auto it = counts.find(requests.front().second);
			if (--it->second == 0) {
				counts.erase(it);
			}
			requests.pop_front();
		}
	}
	uint64_t lastTick = 0;
	std::mutex mutex;
	Snapshot stats;
	std::deque<std::pair<uint64_t, uint32_t>> requests;
	std::map<uint32_t, uint64_t> counts;
};
} // namespace Kademlia
#endif
