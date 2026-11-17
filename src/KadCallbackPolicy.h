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

#include <cstddef>
#include <cstdint>
#include <map>

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

// The next file to publish as a Kad source, or nullptr. `waits` holds the due files that wait for
// their last storing nodes, with when each first waited. They come first on every tick, because
// the cursor reaches a file again only a pass later. `files` gives Find(key), Next() from the
// cursor, Key(file), Waits(file, now, waitingSince), Waited(file) and Publish(file).
template <typename Key, typename Files>
auto NextKadSourcePublish(std::map<Key, uint32_t> &waits, uint32_t now, size_t maxChecks, Files &files)
	-> decltype(files.Next())
{
	for (auto it = waits.begin(); it != waits.end();) {
		auto *file = files.Find(it->first);
		if (file && files.Waits(*file, now, it->second)) {
			++it;
			continue;
		}
		it = waits.erase(it);
		if (file && files.Publish(*file)) {
			return file;
		}
	}
	for (size_t i = 0; i < maxChecks; ++i) {
		auto *file = files.Next();
		if (!file) {
			break;
		}
		if (waits.count(files.Key(*file))) {
			continue;
		}
		if (files.Waits(*file, now, 0)) {
			waits.emplace(files.Key(*file), now);
			files.Waited(*file);
			continue;
		}
		if (files.Publish(*file)) {
			return file;
		}
	}
	return nullptr;
}

// The next due keyword whose publish starts, checking at most `maxChecks` from the list cursor, or
// nullptr. `keywords` gives Next() from the cursor, Due(keyword, now) and Start(keyword, now).
template <typename Keywords>
auto NextKeywordPublish(uint32_t now, size_t maxChecks, Keywords &keywords) -> decltype(keywords.Next())
{
	for (size_t i = 0; i < maxChecks; ++i) {
		auto *keyword = keywords.Next();
		if (!keyword) {
			break;
		}
		if (keywords.Due(*keyword, now) && keywords.Start(*keyword, now)) {
			return keyword;
		}
	}
	return nullptr;
}

} // namespace Kademlia

#endif // KAD_CALLBACK_POLICY_H
