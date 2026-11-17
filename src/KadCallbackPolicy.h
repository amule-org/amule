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

#include <cstdint>

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

} // namespace Kademlia

#endif // KAD_CALLBACK_POLICY_H
