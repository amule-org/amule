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

#ifndef KAD_SERVED_BUDDY_POLICY_H
#define KAD_SERVED_BUDDY_POLICY_H

#include "SourceSearchLimits.h" // Needed for CPrefRange

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Kademlia
{

// How many firewalled nodes an open node serves as their Kad buddy. Stock eMule serves one.
constexpr uint32_t MinServedBuddiesSetting = 4;
constexpr uint32_t DefaultServedBuddies = 8;
constexpr uint32_t MaxServedBuddies = 16;
constexpr CPrefRange KAD_SERVED_BUDDIES{ MinServedBuddiesSetting, DefaultServedBuddies, MaxServedBuddies };

// Relayed bytes a served buddy may receive in a minute. A busy served node needs a few KB.
constexpr uint64_t ServedBuddyRelayBytesPerMinute = 64 * 1024;

// The largest OP_REASKCALLBACKUDP the format can carry: BuddyID (16), file hash (16), part count
// (2), one bit for each of 65,535 parts (8,192) and complete sources (2). It is relayed whole.
constexpr std::size_t MaxReaskRelaySize = 8228;

// The NAT-T bits a requester may set in the connect options byte of a 35-byte buddy request:
// 0x80 for uTP (eMuleAI), 0x40 for QUIC (eMuleAI, eMuleQt).
constexpr uint8_t NatTConnectOptions = 0x80 | 0x40;

inline bool IsNatTCapableRequest(uint8_t connectOptions)
{
	return (connectOptions & NatTConnectOptions) != 0;
}

// The setting, capped at a tenth of the connection limit: each served buddy holds a connection.
// Never fewer than one, the stock count.
inline uint32_t EffectiveServedBuddyLimit(uint32_t setting, uint32_t maxConnections)
{
	const uint32_t wanted = std::clamp(setting, MinServedBuddiesSetting, MaxServedBuddies);
	return std::max<uint32_t>(1, std::min(wanted, maxConnections / 10));
}

// Slots kept for NAT-T capable requesters: a third, rounded down. A node that does not relay the
// Kad rendezvous keeps none, because a NAT-T node would gain nothing from it.
inline uint32_t ReservedNatTSlots(uint32_t limit, bool relaysKadRendezvous)
{
	return relaysKadRendezvous ? limit / 3 : 0;
}

// Whether one more buddy fits. NAT-T nodes fill the kept slots first and may also take an open
// one; other nodes take only open slots, so no healthy buddy is ever evicted to make room.
inline bool AdmitsServedBuddy(
	uint32_t served, uint32_t servedNatT, uint32_t limit, uint32_t reserved, bool requesterNatT)
{
	if (served >= limit) {
		return false;
	}
	if (requesterNatT) {
		return true;
	}
	const uint32_t natTInOpenSlots = servedNatT > reserved ? servedNatT - reserved : 0;
	const uint32_t openUsed = (served - servedNatT) + natTInOpenSlots;
	return openUsed < limit - std::min(reserved, limit);
}

} // namespace Kademlia

#endif // KAD_SERVED_BUDDY_POLICY_H
