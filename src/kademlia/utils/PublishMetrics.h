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
#include <set>
#include <utility>
#include <vector>

namespace Kademlia
{
// Packets of one kind queued to each destination IP in a rolling 60 s window. The receivers'
// limits in PacketTracking.cpp are leaky buckets, so this window is a close approximation.
class CPerIPRate
{
public:
	static constexpr size_t MaxSamples = 65536;
	explicit CPerIPRate(std::vector<uint64_t> thresholds)
	: m_thresholds(std::move(thresholds))
	, m_reached(m_thresholds.size())
	{
	}
	// Returns true when a logged value changed.
	bool Add(uint32_t ip, uint64_t now)
	{
		Expire(now);
		if (m_samples.size() == MaxSamples) {
			++omitted;
			return true;
		}
		m_samples.emplace_back(now, ip);
		const uint64_t count = ++m_counts[ip];
		bool changed = false;
		if (count > peak) {
			peak = count;
			changed = true;
		}
		for (size_t i = 0; i < m_thresholds.size(); ++i) {
			if (count == m_thresholds[i]) {
				changed |= m_reached[i].insert(ip).second;
			}
		}
		return changed;
	}
	void Expire(uint64_t now)
	{
		while (!m_samples.empty() && now - m_samples.front().first >= 60000) {
			auto it = m_counts.find(m_samples.front().second);
			if (--it->second == 0) {
				m_counts.erase(it);
			}
			m_samples.pop_front();
		}
	}
	// The IPs that reached each threshold since the previous call, in threshold order.
	std::vector<std::set<uint32_t>> TakeReached()
	{
		std::vector<std::set<uint32_t>> reached(m_thresholds.size());
		reached.swap(m_reached);
		return reached;
	}
	uint64_t peak = 0, omitted = 0;

private:
	std::vector<uint64_t> m_thresholds;
	std::vector<std::set<uint32_t>> m_reached;
	std::deque<std::pair<uint64_t, uint32_t>> m_samples;
	std::map<uint32_t, uint64_t> m_counts;
};

// Main-thread diagnostics, called only while the Kademlia Publish category is enabled.
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
	void Queued(uint32_t ip, uint64_t now)
	{
		++queued;
		++changes;
		publishRequests.Add(ip, now);
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
		const auto replies = Acknowledged(trace);
		acknowledged += replies;
		if (!completed) {
			++cancelled;
			return;
		}
		++finished;
		zeroAcknowledgements += replies == 0;
		durationMs += elapsed;
		maxDurationMs = std::max(maxDurationMs, elapsed);
	}
	void LookupQueued(uint32_t ip, uint64_t now)
	{
		if (lookupRequests.Add(ip, now)) {
			++changes;
		}
	}
	bool ReportDue(uint64_t now)
	{
		lookupRequests.Expire(now);
		publishRequests.Expire(now);
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
		reportedLookupIPs = lookupRequests.TakeReached();
		reportedPublishIPs = publishRequests.TakeReached();
		return true;
	}
	uint64_t attempts = 0, startFailures = 0, starts = 0;
	uint64_t queued = 0, acknowledged = 0, finished = 0, zeroAcknowledgements = 0, cancelled = 0;
	uint64_t durationMs = 0, maxDurationMs = 0;
	// Receivers drop a KADEMLIA2_REQ above 10 a minute and a KADEMLIA2_PUBLISH_SOURCE_REQ above 2.
	CPerIPRate lookupRequests{ { 6, 8, 11 } };
	CPerIPRate publishRequests{ { 2, 3 } };
	// Set by a due report: the IPs that reached each threshold since the previous one.
	std::vector<std::set<uint32_t>> reportedLookupIPs, reportedPublishIPs;

private:
	bool reportStarted = false;
	uint64_t lastReport = 0, changes = 0, reportedChanges = 0;
};
} // namespace Kademlia
#endif
