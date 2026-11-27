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

#ifndef KAD_CALLBACK_POLICY_H
#define KAD_CALLBACK_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>

#include "SourceSearchLimits.h" // Needed for CPrefRange

namespace Kademlia
{

// UDP reachability must be verified before a TCP-firewalled node advertises callbacks.
inline bool DirectCallbackAvailable(bool tcpFirewalled, bool udpFirewalled, bool udpVerified)
{
	return tcpFirewalled && !udpFirewalled && udpVerified;
}

inline bool NeedsBuddy(bool tcpFirewalled, bool udpFirewalled, bool udpVerified)
{
	return tcpFirewalled && (udpFirewalled || !udpVerified);
}

// A changed callback route must replace its published address without waiting for the timer.
inline bool CanPublishSource(bool needsBuddy,
	uint32_t buddyIP,
	uint32_t lastBuddyIP,
	uint32_t nextPublishTime,
	uint32_t now,
	uint16_t buddyPort,
	uint16_t lastBuddyPort)
{
	if (needsBuddy && buddyIP == 0) {
		return false;
	}
	const uint32_t routeBuddyIP = needsBuddy ? buddyIP : 0;
	const uint16_t routeBuddyPort = needsBuddy ? buddyPort : 0;
	return routeBuddyIP != lastBuddyIP || routeBuddyPort != lastBuddyPort || now >= nextPublishTime;
}

// A due source publish waits while a node it last stored on has no source store left this minute,
// for at most `maxWait` seconds from `waitingSince` (0 before its first wait). A file due early
// after a buddy change never waits. `seedOutOfBudget` is called only for a due file.
template <typename SeedOutOfBudget>
inline bool KadSourcePublishWaits(uint32_t nextPublishTime,
	uint32_t now,
	uint32_t waitingSince,
	uint32_t maxWait,
	const SeedOutOfBudget &seedOutOfBudget)
{
	if (now < nextPublishTime || (waitingSince != 0 && now - waitingSince >= maxWait)) {
		return false;
	}
	return seedOutOfBudget();
}

// Due republishes, earliest first, and the share of starts kept for items never published.
// Entries are hints: the item's own publish time decides, so an item republished early or no
// longer shared leaves a stale entry that is skipped.
template <typename Key> class CKadPublishOrder
{
public:
	// Every 10th start goes to an item never published, while one is left, so new items still
	// go out when republishes fill every slot.
	static constexpr uint32_t FirstPublishShare = 10;

	void Add(uint32_t due, const Key &key) { m_due.emplace(due, key); }

	bool PopDue(uint32_t now, Key &key)
	{
		if (m_due.empty() || m_due.begin()->first > now) {
			return false;
		}
		key = m_due.begin()->second;
		m_due.erase(m_due.begin());
		return true;
	}

	bool FirstPublishTurn() const { return m_sinceFirst + 1 >= FirstPublishShare; }
	void Started(bool first) { m_sinceFirst = first ? 0 : m_sinceFirst + 1; }

private:
	std::multimap<uint32_t, Key> m_due;
	uint32_t m_sinceFirst = 0;
};

// The next file to publish as a Kad source, or nullptr. Due republishes go first, most overdue
// first, so files published early in a long first pass do not wait for its end. `waits` holds the
// due files that wait for their last storing nodes, with when each first waited. The cursor then
// finds what the order does not hold: files never published, and files due early after a route
// change. `files` gives Find(key), Next() from the cursor, Key(file), PublishTime(file) (its next
// publish time, 0 if never published), Waits(file, now, waitingSince), Waited(file) and
// Publish(file).
template <typename Key, typename Files>
auto NextKadSourcePublish(std::map<Key, uint32_t> &waits,
	CKadPublishOrder<Key> &order,
	uint32_t now,
	size_t maxChecks,
	Files &files) -> decltype(files.Next())
{
	const auto publish = [&](decltype(files.Next()) file) {
		const bool first = files.PublishTime(*file) == 0;
		if (!files.Publish(*file)) {
			return false;
		}
		order.Add(files.PublishTime(*file), files.Key(*file));
		order.Started(first);
		return true;
	};
	if (order.FirstPublishTurn()) {
		for (size_t i = 0; i < maxChecks; ++i) {
			auto *file = files.Next();
			if (!file) {
				break;
			}
			if (files.PublishTime(*file) == 0 && !waits.count(files.Key(*file)) &&
				publish(file)) {
				return file;
			}
		}
	}
	for (auto it = waits.begin(); it != waits.end();) {
		auto *file = files.Find(it->first);
		if (file && files.Waits(*file, now, it->second)) {
			++it;
			continue;
		}
		it = waits.erase(it);
		if (file && publish(file)) {
			return file;
		}
	}
	const auto waitOrPublish = [&](decltype(files.Next()) file) {
		if (files.Waits(*file, now, 0)) {
			waits.emplace(files.Key(*file), now);
			files.Waited(*file);
			return false;
		}
		return publish(file);
	};
	Key key;
	for (size_t i = 0; i < maxChecks && order.PopDue(now, key); ++i) {
		auto *file = files.Find(key);
		if (!file || waits.count(key) || files.PublishTime(*file) == 0 ||
			files.PublishTime(*file) > now) {
			continue;
		}
		if (waitOrPublish(file)) {
			return file;
		}
	}
	for (size_t i = 0; i < maxChecks; ++i) {
		auto *file = files.Next();
		if (!file) {
			break;
		}
		if (!waits.count(files.Key(*file)) && waitOrPublish(file)) {
			return file;
		}
	}
	return nullptr;
}

// The next due keyword whose publish starts, or nullptr. As for sources, due republishes go
// first and every 10th start goes to a keyword never published. A keyword that fails to start
// leaves the order; the cursor tries it again. `keywords` gives Find(key), Next() from the list
// cursor, Key(keyword), PublishTime(keyword) (0 if never published), Due(keyword, now) and
// Start(keyword, now).
template <typename Key, typename Keywords>
auto NextKeywordPublish(CKadPublishOrder<Key> &order, uint32_t now, size_t maxChecks, Keywords &keywords)
	-> decltype(keywords.Next())
{
	const auto start = [&](decltype(keywords.Next()) keyword) {
		const bool first = keywords.PublishTime(*keyword) == 0;
		if (!keywords.Start(*keyword, now)) {
			return false;
		}
		order.Add(keywords.PublishTime(*keyword), keywords.Key(*keyword));
		order.Started(first);
		return true;
	};
	if (order.FirstPublishTurn()) {
		for (size_t i = 0; i < maxChecks; ++i) {
			auto *keyword = keywords.Next();
			if (!keyword) {
				break;
			}
			if (keywords.PublishTime(*keyword) == 0 && start(keyword)) {
				return keyword;
			}
		}
	}
	Key key;
	for (size_t i = 0; i < maxChecks && order.PopDue(now, key); ++i) {
		auto *keyword = keywords.Find(key);
		if (keyword && keywords.PublishTime(*keyword) != 0 && keywords.PublishTime(*keyword) <= now &&
			start(keyword)) {
			return keyword;
		}
	}
	for (size_t i = 0; i < maxChecks; ++i) {
		auto *keyword = keywords.Next();
		if (!keyword) {
			break;
		}
		if (keywords.Due(*keyword, now) && start(keyword)) {
			return keyword;
		}
	}
	return nullptr;
}

// In lab runs on the public Kad, one slot of seeded lookups republished about 1,650 files per 5 h,
// or 4,900 keywords per 24 h. These counts leave a margin.
constexpr uint32_t KadFilesPerSourceSlot = 1500;
constexpr uint32_t KadKeywordsPerKeywordSlot = 4500;
// Concurrent publish lookups. The minimum is the old fixed count; the setting caps the automatic
// count, and the default is the highest count tested live.
constexpr CPrefRange KAD_SOURCE_PUBLISH_SLOTS{ 3, 12, 32 };
constexpr CPrefRange KAD_KEYWORD_PUBLISH_SLOTS{ 2, 8, 32 };

// Slots needed to republish `items` once per interval, never under `minSlots`.
inline uint32_t KadPublishSlotsNeeded(size_t items, uint32_t perSlot, uint32_t minSlots)
{
	const size_t needed = (items + perSlot - 1) / perSlot;
	return needed > minSlots ? static_cast<uint32_t>(needed) : minSlots;
}

// The publish slots, `needed` capped at `max`. `logCap` runs each time the cap starts to hold.
template <typename LogCap>
uint32_t CappedKadPublishSlots(uint32_t needed, uint32_t max, bool &capped, const LogCap &logCap)
{
	if (needed > max && !capped) {
		logCap();
	}
	capped = needed > max;
	return needed > max ? max : needed;
}

// One start per 2 s publish tick allows 30 a minute. A source slot finishes about 6 seeded lookups
// a minute and a keyword slot about 3.4, so one start keeps about 5 source or 8 keyword slots busy.
constexpr uint32_t KadSourceSlotsPerStart = 3;
constexpr uint32_t KadKeywordSlotsPerStart = 8;

// Lookups to start in one tick: one per `slotsPerStart` slots, never more than are free.
inline uint32_t KadPublishStartsNow(uint32_t slots, uint32_t running, uint32_t slotsPerStart)
{
	if (running >= slots) {
		return 0;
	}
	return std::min(slots - running, (slots + slotsPerStart - 1) / slotsPerStart);
}

} // namespace Kademlia

#endif // KAD_CALLBACK_POLICY_H
