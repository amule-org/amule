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

#ifndef KAD_BUDDY_STATS_H
#define KAD_BUDDY_STATS_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Kademlia
{

// The largest total one key reached in a fixed one-minute window. Keys past the cap in one
// window are not counted, so forged sender addresses cannot grow the map.
class CMinutePeak
{
public:
	static constexpr std::size_t MaxKeys = 1024;
	static constexpr uint64_t WindowMs = 60 * 1000;

	void Add(uint32_t key, uint64_t amount, uint64_t now)
	{
		if (now < m_windowStart || now - m_windowStart >= WindowMs) {
			m_totals.clear();
			m_windowStart = now;
		}
		auto it = m_totals.find(key);
		if (it == m_totals.end()) {
			if (m_totals.size() >= MaxKeys) {
				return;
			}
			it = m_totals.emplace(key, 0).first;
		}
		it->second += amount;
		m_peak = std::max(m_peak, it->second);
	}

	uint64_t Peak() const { return m_peak; }

private:
	std::unordered_map<uint32_t, uint64_t> m_totals;
	uint64_t m_windowStart = 0;
	uint64_t m_peak = 0;
};

// What a serving node relays to its buddy, and what it drops.
struct CBuddyRelayCounts
{
	uint64_t callbacks = 0;
	uint64_t callbackBytes = 0;
	uint64_t callbacksNoBuddy = 0;
	uint64_t callbacksMismatched = 0;
	uint64_t callbacksRateLimited = 0;
	uint64_t reasks = 0;
	uint64_t reaskBytes = 0;
	uint64_t reasksNoBuddy = 0;
	uint64_t reasksMismatched = 0;
	uint64_t reasksMalformed = 0;
};

// Counts since start. Times are monotonic milliseconds.
struct CBuddyStats
{
	// We are open, and a firewalled node uses us as its buddy.
	struct Serving
	{
		uint64_t requestsReceived = 0;
		uint64_t requestsOffered = 0;
		uint64_t requestsRefusedFirewalled = 0;
		uint64_t requestsRefusedFull = 0;
		uint64_t requestsRefusedConflict = 0;
		uint64_t requestsRateLimited = 0;
		// Offered, but the node did not connect in time, or another buddy connected first.
		uint64_t offersExpired = 0;
		uint64_t offersSuperseded = 0;
		uint64_t started = 0;
		uint64_t releasedExtra = 0;
		uint64_t ended = 0;
		uint64_t timedOut = 0;
		uint64_t endedMs = 0; // Time served by buddies that have ended.
		CBuddyRelayCounts relays;
		CMinutePeak relayBytesPerBuddy;
		CMinutePeak callbackRequestsPerClient;
	} serving;

	// We are firewalled and need a buddy.
	struct Own
	{
		uint64_t searches = 0;
		uint64_t requestsSent = 0;
		uint64_t requestsSkipped = 0; // The node failed us within the cooldown.
		uint64_t offersReceived = 0;
		uint64_t connectAttempts = 0;
		uint64_t connectFailures = 0;
		uint64_t connected = 0;
		uint64_t ended = 0;
		uint64_t timedOut = 0;
		uint64_t endedMs = 0; // Time with buddies that have ended.
		uint64_t callbacksReceived = 0;
		uint64_t reasksReceived = 0;
		uint64_t neededMs = 0;
		uint64_t withoutBuddyMs = 0;
	} own;
};

// One log line for the relays since `before`, or an empty string when nothing changed.
inline std::string FormatBuddyRelaySummary(const CBuddyRelayCounts &now, const CBuddyRelayCounts &before)
{
	std::string relayed;
	const auto relay = [&relayed](uint64_t count, uint64_t bytes, const char *what) {
		if (count != 0) {
			relayed += (relayed.empty() ? "" : ", ") + std::string(what) + " " +
				   std::to_string(count) + " (" + std::to_string(bytes) + " B)";
		}
	};
	std::string dropped;
	const auto drop = [&dropped](uint64_t count, const char *what) {
		if (count != 0) {
			dropped += (dropped.empty() ? "" : ", ") + std::string(what) + " " +
				   std::to_string(count);
		}
	};
	relay(now.callbacks - before.callbacks, now.callbackBytes - before.callbackBytes, "callbacks");
	relay(now.reasks - before.reasks, now.reaskBytes - before.reaskBytes, "reasks");
	drop(now.callbacksNoBuddy - before.callbacksNoBuddy, "callbacks without a buddy");
	drop(now.callbacksMismatched - before.callbacksMismatched, "callbacks for another buddy");
	drop(now.callbacksRateLimited - before.callbacksRateLimited, "callbacks over the rate limit");
	drop(now.reasksNoBuddy - before.reasksNoBuddy, "reasks without a buddy");
	drop(now.reasksMismatched - before.reasksMismatched, "reasks for another buddy");
	drop(now.reasksMalformed - before.reasksMalformed, "malformed reasks");
	if (relayed.empty() && dropped.empty()) {
		return {};
	}
	std::string line = "Relays in the last minute: " + (relayed.empty() ? std::string("none") : relayed);
	if (!dropped.empty()) {
		line += ". Dropped: " + dropped;
	}
	return line;
}

} // namespace Kademlia

#endif // KAD_BUDDY_STATS_H
