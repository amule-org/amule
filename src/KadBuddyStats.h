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

#include "KadBuddyIdLayout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Kademlia
{

// The largest total one key reached in a fixed one-minute window. Keys past the cap in one
// window are not counted, so forged sender addresses cannot grow the map.
class CMinutePeak
{
public:
	static constexpr std::size_t MaxKeys = 1024;
	static constexpr uint64_t WindowMs = 60 * 1000;

	// Returns the key's total in this window, or 0 when the cap left it out.
	uint64_t Add(uint32_t key, uint64_t amount, uint64_t now)
	{
		if (now < m_windowStart || now - m_windowStart >= WindowMs) {
			m_totals.clear();
			m_windowStart = now;
		}
		auto it = m_totals.find(key);
		if (it == m_totals.end()) {
			if (m_totals.size() >= MaxKeys) {
				return 0;
			}
			it = m_totals.emplace(key, 0).first;
		}
		it->second += amount;
		m_peak = std::max(m_peak, it->second);
		return it->second;
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
	uint64_t reasksMalformed = 0; // Too short to name a buddy, or larger than any reask.
	uint64_t callbacksOverBudget = 0;
	uint64_t reasksOverBudget = 0;
	// Log only: the drops above whose BuddyID belongs to a buddy that left.
	uint64_t callbacksDeparted = 0;
	uint64_t reasksDeparted = 0;
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
		uint64_t requestsRefusedSameIP = 0; // We already serve, or offered to, that address.
		uint64_t requestsNatT = 0;          // Received with a NAT-T bit in the connect options.
		// Offered, but the node did not connect in time, another buddy took the last slot, or
		// the node linked through another offer.
		uint64_t offersExpired = 0;
		uint64_t offersSuperseded = 0;
		uint64_t offersDuplicate = 0;
		uint64_t offersEvicted = 0; // Replaced by a newer request while the pending list was full.
		uint64_t started = 0;
		uint64_t releasedExtra = 0;
		uint64_t ended = 0;
		uint64_t timedOut = 0;
		uint64_t endedMs = 0; // Time served by buddies that have ended.
		CBuddyRelayCounts relays;
		uint64_t peakRelayBytesPerMinute = 0; // To one served buddy.
		CMinutePeak callbackRequestsPerClient;
	} serving;

	// We are firewalled and need a buddy.
	struct Own
	{
		uint64_t searches = 0;
		uint64_t searchesHeld = 0; // A due search waited for a shared file.
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

using BuddyId = std::array<uint8_t, 16>;

inline BuddyId MakeBuddyId(const uint8_t *bytes)
{
	BuddyId id;
	std::memcpy(id.data(), bytes, id.size());
	return id;
}

// Upper-case hex of the 16 bytes as stored, so log lines from different runs can be matched.
inline std::string BuddyIdHex(const uint8_t *bytes)
{
	static const char digits[] = "0123456789ABCDEF";
	std::string hex;
	hex.reserve(32);
	for (std::size_t i = 0; i < 16; ++i) {
		hex += digits[bytes[i] >> 4];
		hex += digits[bytes[i] & 0x0F];
	}
	return hex;
}

// "42 s", "5 min 2 s" or "3 h 12 min".
inline std::string FormatBuddyDuration(uint64_t ms)
{
	const uint64_t seconds = ms / 1000;
	if (seconds < 60) {
		return std::to_string(seconds) + " s";
	}
	if (seconds < 3600) {
		return std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s";
	}
	return std::to_string(seconds / 3600) + " h " + std::to_string(seconds % 3600 / 60) + " min";
}

// One served buddy while it stays linked.
struct CServedBuddyLink
{
	const void *client = nullptr; // Identifies the link; never dereferenced.
	BuddyId buddyId{};
	std::string address;
	std::string software;
	uint64_t linkedAt = 0;
	uint32_t linkNumber = 1; // Counts earlier links of the same node that we still remember.
	uint64_t callbacks = 0;
	uint64_t callbackBytes = 0;
	uint64_t reasks = 0;
	uint64_t reaskBytes = 0;
	uint64_t minuteStart = 0;
	uint64_t minuteBytes = 0;
	uint64_t peakMinuteBytes = 0;
	uint32_t ip = 0; // For one served buddy per address.
	bool natT = false;
	// When each callback sender last asked for this buddy.
	std::unordered_map<uint32_t, uint64_t> callbackSenders;
	static constexpr std::size_t MaxCallbackSenders = 256;

	// Whether `bytes` more fit in `budget` for the current one-minute window.
	bool HasBudgetFor(uint64_t bytes, uint64_t budget, uint64_t now) const
	{
		const bool sameWindow = now >= minuteStart && now - minuteStart < CMinutePeak::WindowMs;
		return (sameWindow ? minuteBytes : 0) + bytes <= budget;
	}

	// One callback request a minute from each sender for this buddy, as stock eMule allows for its
	// only buddy. A table full of live senders lets the request through: the byte budget still
	// bounds what we relay.
	bool AllowCallbackFrom(uint32_t sender, uint64_t now)
	{
		const auto known = callbackSenders.find(sender);
		if (known != callbackSenders.end()) {
			if (now >= known->second && now - known->second < CMinutePeak::WindowMs) {
				return false;
			}
			known->second = now;
			return true;
		}
		if (callbackSenders.size() >= MaxCallbackSenders) {
			for (auto it = callbackSenders.begin(); it != callbackSenders.end();) {
				it = (now < it->second || now - it->second >= CMinutePeak::WindowMs)
					     ? callbackSenders.erase(it)
					     : std::next(it);
			}
			if (callbackSenders.size() >= MaxCallbackSenders) {
				return true;
			}
		}
		callbackSenders.emplace(sender, now);
		return true;
	}

	// Returns the bytes relayed to this buddy in the current one-minute window.
	uint64_t Relay(bool callback, uint64_t bytes, uint64_t now)
	{
		if (callback) {
			++callbacks;
			callbackBytes += bytes;
		} else {
			++reasks;
			reaskBytes += bytes;
		}
		if (now < minuteStart || now - minuteStart >= CMinutePeak::WindowMs) {
			minuteStart = now;
			minuteBytes = 0;
		}
		minuteBytes += bytes;
		peakMinuteBytes = std::max(peakMinuteBytes, minuteBytes);
		return minuteBytes;
	}
};

// "callbacks 3 (114 B), reasks 0 (0 B), busiest minute 76 B"
inline std::string FormatServedBuddyRelays(const CServedBuddyLink &link)
{
	return "callbacks " + std::to_string(link.callbacks) + " (" + std::to_string(link.callbackBytes) +
	       " B), reasks " + std::to_string(link.reasks) + " (" + std::to_string(link.reaskBytes) +
	       " B), busiest minute " + std::to_string(link.peakMinuteBytes) + " B";
}

struct CDepartedBuddy
{
	BuddyId buddyId{};
	std::string address;
	uint64_t endedAt = 0;
	uint32_t links = 0;
};

// Served buddies that left, so drops for their BuddyID can be told apart from unknown ones, and a
// node that links again is recognised. Entries hold copies only, so a lookup never touches a client.
class CDepartedBuddies
{
public:
	// Downloaders keep a source and its BuddyID past the 5 h Kad source lifetime: live callbacks came
	// up to about 8 h after the buddy left.
	static constexpr uint64_t MaxAgeMs = 8 * 60 * 60 * 1000ULL;

	// Keeps at most `capacity` entries, the newest ones. A node already listed moves to the end.
	void Record(const CDepartedBuddy &buddy, std::size_t capacity)
	{
		m_buddies.erase(std::remove_if(m_buddies.begin(),
					m_buddies.end(),
					[&buddy](const CDepartedBuddy &listed) {
						return listed.buddyId == buddy.buddyId;
					}),
			m_buddies.end());
		Prune(buddy.endedAt);
		m_buddies.push_back(buddy);
		if (m_buddies.size() > capacity) {
			m_buddies.erase(
				m_buddies.begin(), m_buddies.end() - static_cast<std::ptrdiff_t>(capacity));
		}
	}

	// The newest match in either BuddyID layout, or nothing.
	std::optional<CDepartedBuddy> Find(const uint8_t *buddyId, uint64_t now) const
	{
		for (auto it = m_buddies.rbegin(); it != m_buddies.rend(); ++it) {
			if (Age(*it, now) < MaxAgeMs &&
				MatchesBuddyIdEitherLayout(buddyId, it->buddyId.data())) {
				return *it;
			}
		}
		return std::nullopt;
	}

	std::size_t Size() const { return m_buddies.size(); }

	static uint64_t Age(const CDepartedBuddy &buddy, uint64_t now)
	{
		return now > buddy.endedAt ? now - buddy.endedAt : 0;
	}

private:
	void Prune(uint64_t now)
	{
		m_buddies.erase(
			std::remove_if(m_buddies.begin(),
				m_buddies.end(),
				[now](const CDepartedBuddy &listed) { return Age(listed, now) >= MaxAgeMs; }),
			m_buddies.end());
	}

	std::vector<CDepartedBuddy> m_buddies;
};

// One log line for the relays since `before`, or an empty string when nothing changed.
// `departedNote` names the latest buddy that left whose BuddyID a drop carried.
inline std::string FormatBuddyRelaySummary(
	const CBuddyRelayCounts &now, const CBuddyRelayCounts &before, const std::string &departedNote = {})
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
	// Drops without a matching buddy, split by whether the BuddyID belongs to one that left.
	const auto unmatched =
		[&drop](uint64_t total, uint64_t departed, const char *left, const char *unknown) {
			drop(departed, left);
			drop(total > departed ? total - departed : 0, unknown);
		};
	relay(now.callbacks - before.callbacks, now.callbackBytes - before.callbackBytes, "callbacks");
	relay(now.reasks - before.reasks, now.reaskBytes - before.reaskBytes, "reasks");
	const uint64_t callbacksDeparted = now.callbacksDeparted - before.callbacksDeparted;
	const uint64_t reasksDeparted = now.reasksDeparted - before.reasksDeparted;
	unmatched(now.callbacksNoBuddy - before.callbacksNoBuddy + now.callbacksMismatched -
			  before.callbacksMismatched,
		callbacksDeparted,
		"callbacks for a buddy that left",
		"callbacks for an unknown buddy");
	drop(now.callbacksRateLimited - before.callbacksRateLimited, "callbacks over the rate limit");
	unmatched(now.reasksNoBuddy - before.reasksNoBuddy + now.reasksMismatched - before.reasksMismatched,
		reasksDeparted,
		"reasks for a buddy that left",
		"reasks for an unknown buddy");
	drop(now.reasksMalformed - before.reasksMalformed, "malformed reasks");
	drop(now.callbacksOverBudget - before.callbacksOverBudget, "callbacks over a buddy's byte budget");
	drop(now.reasksOverBudget - before.reasksOverBudget, "reasks over a buddy's byte budget");
	if (relayed.empty() && dropped.empty()) {
		return {};
	}
	std::string line = "Relays in the last minute: " + (relayed.empty() ? std::string("none") : relayed);
	if (!dropped.empty()) {
		line += ". Dropped: " + dropped;
		if (callbacksDeparted + reasksDeparted != 0 && !departedNote.empty()) {
			line += ". Latest buddy that left: " + departedNote;
		}
	}
	return line;
}

} // namespace Kademlia

#endif // KAD_BUDDY_STATS_H
