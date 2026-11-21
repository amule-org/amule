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

#ifndef KAD_POINTER_CACHE_H
#define KAD_POINTER_CACHE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

// The receiving side of the buddy pointer (#2513, emule-qt ipv6-spec §5.6.1): when a callback
// through the buddy a marked record names goes unanswered, the downloader looks up the pointer at
// ~sourceID and asks the buddies it names. One lookup serves every copy of the source.

//! A pointer names at most this many buddies we try; storers that missed a republish keep older ones.
constexpr unsigned KAD_POINTER_MAX_BUDDIES = 3;
//! How long a pointer lookup runs (SEARCHFILE_LIFETIME).
constexpr uint32_t KAD_POINTER_LOOKUP_SECONDS = 45;
//! Reasks in a row a source whose pointer gave no buddy is kept for: about an hour by default, the
//! median time a LowID spent without a buddy in our logs.
constexpr unsigned KAD_POINTER_MAX_MISSES = 4;
//! How long a lookup that found a pointer is reused.
constexpr uint32_t KAD_POINTER_HIT_SECONDS = 3600;
//! Pointer lookups run at once at most, as eMuleQt does (§5.6.1).
constexpr unsigned KAD_POINTER_MAX_RUNNING = 5;

//! Whether a source whose pointer gave no buddy `misses` times in a row is kept for another try.
inline bool ShouldRetryKadPointer(unsigned misses) noexcept
{
	return misses < KAD_POINTER_MAX_MISSES;
}

//! A buddy a pointer names. The ID is in the layout the source's other records give it.
struct SKadPointerBuddy
{
	uint32_t ip = 0;
	uint16_t port = 0;
	std::array<uint8_t, 16> id{};
};

//! What a pointer lookup returned from one storer.
struct SKadPointerRecord
{
	uint8_t type = 0;
	//! The record is under the source ID we looked up.
	bool sameSource = false;
	//! TAG_SOURCEIP, as the storer recorded it; more than one is refused.
	uint32_t sourceIP = 0;
	unsigned sourceIPs = 0;
	bool hasBuddyID = false;
	SKadPointerBuddy buddy;
};

enum class EKadPointerVerdict
{
	Accepted,
	NotTypeThree,
	OtherSource,
	OtherSourceIP,
	NoBuddyRoute,
};

/**
 * §5.6.1 and §5.6.2: a pointer is used only as a type 3 record under the source ID, with a buddy
 * address and buddy ID, from the same public IP as the record that sent us looking. Anyone can
 * store a record under any key; the storer records the IP it came from, and only that is checked.
 */
inline EKadPointerVerdict CheckKadPointerRecord(const SKadPointerRecord &record, uint32_t recordIP) noexcept
{
	if (record.type != 3) {
		return EKadPointerVerdict::NotTypeThree;
	}
	if (!record.sameSource) {
		return EKadPointerVerdict::OtherSource;
	}
	if (record.sourceIPs != 1 || record.sourceIP != recordIP) {
		return EKadPointerVerdict::OtherSourceIP;
	}
	const bool zeroID =
		std::all_of(record.buddy.id.begin(), record.buddy.id.end(), [](uint8_t b) { return b == 0; });
	if (!record.hasBuddyID || zeroID || record.buddy.ip == 0 || record.buddy.port == 0) {
		return EKadPointerVerdict::NoBuddyRoute;
	}
	return EKadPointerVerdict::Accepted;
}

//! What a source whose buddy route failed does next with the pointer.
enum class EKadPointerStep
{
	//! No lookup is current: start one, and wait.
	Look,
	//! The lookup runs and has found nothing usable yet.
	Wait,
	//! Ask this buddy.
	Use,
	//! Nothing left from the pointer: the buddy route has failed.
	Exhausted,
};

struct SKadPointerNext
{
	EKadPointerStep step = EKadPointerStep::Exhausted;
	SKadPointerBuddy buddy;
	//! Where the source looks next time, past the buddy it was given.
	unsigned next = 0;
};

/**
 * Pointer lookups by source ID. A found pointer is kept an hour, a miss for the source reask
 * interval: a downloader only comes back at its next reask, so a shorter miss would buy nothing.
 */
class CKadPointerCache
{
public:
	using Key = std::array<uint8_t, 16>;

	/**
	 * The next step for a source at `next` in the buddy list, skipping the buddy it just asked.
	 * A source gets each buddy once a round; once the lookup ended and the list is used up, it is
	 * Exhausted until the entry expires, or Spent() ends a hit whose buddies all failed.
	 */
	SKadPointerNext Next(
		const Key &key, unsigned next, uint32_t askedIP, uint16_t askedPort, uint32_t now) const
	{
		SKadPointerNext result;
		const auto it = m_entries.find(key);
		if (it == m_entries.end() || (!it->second.running && now >= it->second.expires)) {
			result.step = next == 0 ? EKadPointerStep::Look : EKadPointerStep::Exhausted;
			return result;
		}
		const SEntry &entry = it->second;
		for (unsigned i = next; i < entry.buddies.size(); ++i) {
			const SKadPointerBuddy &buddy = entry.buddies[i];
			if (buddy.ip == askedIP && buddy.port == askedPort) {
				continue;
			}
			result.step = EKadPointerStep::Use;
			result.buddy = buddy;
			result.next = i + 1;
			return result;
		}
		result.step = entry.running ? EKadPointerStep::Wait : EKadPointerStep::Exhausted;
		result.next = next;
		return result;
	}

	//! A lookup started for a source whose record came from `recordIP`.
	void Started(const Key &key, uint32_t recordIP)
	{
		SEntry &entry = m_entries[key];
		entry = SEntry();
		entry.running = true;
		entry.recordIP = recordIP;
	}

	//! A record the lookup returned. A buddy is listed once, in the order storers answer.
	EKadPointerVerdict Add(const Key &key, const SKadPointerRecord &record)
	{
		const auto it = m_entries.find(key);
		if (it == m_entries.end() || !it->second.running) {
			return EKadPointerVerdict::OtherSource;
		}
		SEntry &entry = it->second;
		const EKadPointerVerdict verdict = CheckKadPointerRecord(record, entry.recordIP);
		if (verdict != EKadPointerVerdict::Accepted) {
			++entry.refused;
			return verdict;
		}
		++entry.accepted;
		for (const SKadPointerBuddy &known : entry.buddies) {
			if (known.ip == record.buddy.ip && known.port == record.buddy.port) {
				return verdict;
			}
		}
		if (entry.buddies.size() < KAD_POINTER_MAX_BUDDIES) {
			entry.buddies.push_back(record.buddy);
		}
		return verdict;
	}

	struct SOutcome
	{
		//! A lookup for this key was running.
		bool ran = false;
		bool found = false;
		unsigned buddies = 0;
		unsigned accepted = 0;
		unsigned refused = 0;
	};

	//! The lookup ended. A miss is kept `missSeconds`, a hit an hour.
	SOutcome Finished(const Key &key, uint32_t now, uint32_t missSeconds)
	{
		SOutcome outcome;
		const auto it = m_entries.find(key);
		if (it == m_entries.end() || !it->second.running) {
			return outcome;
		}
		SEntry &entry = it->second;
		entry.running = false;
		outcome.ran = true;
		outcome.found = !entry.buddies.empty();
		entry.expires = now + (outcome.found ? KAD_POINTER_HIT_SECONDS : missSeconds);
		outcome.buddies = static_cast<unsigned>(entry.buddies.size());
		outcome.accepted = entry.accepted;
		outcome.refused = entry.refused;
		return outcome;
	}

	/**
	 * A source asked every buddy a found pointer names and none passed on its callback: the
	 * pointer is stale, as when the publisher has a new buddy and pointer since. The hit ends now,
	 * so the source's next reask (and any other copy of it) looks the pointer up again instead of
	 * asking the same buddies until the hour is over. Returns whether a hit was ended.
	 */
	bool Spent(const Key &key, uint32_t now)
	{
		const auto it = m_entries.find(key);
		if (it == m_entries.end() || it->second.running || it->second.buddies.empty() ||
			now >= it->second.expires) {
			return false;
		}
		it->second.expires = now;
		return true;
	}

	//! Drops what has expired.
	void Prune(uint32_t now)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();) {
			if (!it->second.running && now >= it->second.expires) {
				it = m_entries.erase(it);
			} else {
				++it;
			}
		}
	}

	size_t Size() const noexcept { return m_entries.size(); }

	//! Lookups started and not yet finished.
	unsigned Running() const noexcept
	{
		return static_cast<unsigned>(std::count_if(m_entries.begin(),
			m_entries.end(),
			[](const std::pair<const Key, SEntry> &item) { return item.second.running; }));
	}

private:
	struct SEntry
	{
		bool running = false;
		uint32_t recordIP = 0;
		uint32_t expires = 0;
		unsigned accepted = 0;
		unsigned refused = 0;
		std::vector<SKadPointerBuddy> buddies;
	};
	std::map<Key, SEntry> m_entries;
};

#endif // KAD_POINTER_CACHE_H
