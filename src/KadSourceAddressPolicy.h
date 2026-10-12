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

#ifndef KAD_SOURCE_ADDRESS_POLICY_H
#define KAD_SOURCE_ADDRESS_POLICY_H

#include "ContactDialPolicy.h" // Needed for DecideContactRoute
#include "NetworkAddress.h"
#include "NetworkFunctions.h" // Needed for IsLowID
#include "PeerAddressing.h"   // Needed for IsUsableTagIdentity
#include <cstdint>

struct SKadSourceFacts
{
	std::uint8_t sourceType = 0;   // TAG_SOURCETYPE of the result
	CNetworkAddress publishedIPv6; // The "ip6" tag; Absent when missing or malformed
	std::uint32_t ourHelloID = 0;  // The ID our OP_HELLO carries
	bool ipv6Built = false;
	bool familyPermitted = false;
	bool proxyEnabled = false;
};

/**
 * Whether a Kad source result is contacted on the IPv6 address it published.
 *
 * Only sources firewalled on IPv4 qualify: types 3 and 5 are relayed through a serving buddy and 6
 * takes a direct UDP callback, while types 1 and 4 are HighIDs that keep their IPv4 route. Our HELLO
 * must carry a nonzero LowID: eMuleAI keeps such an ID from a peer arriving over IPv6 but replaces
 * any other with one derived from the IPv6 socket, so a HighID would lose its IPv4 callback path at
 * that peer, and 0 is no identity at all. The address is reported by a third party through Kad, so
 * it must name a peer across hosts and must not point us at local services. The dial itself goes
 * through the same gate as every native IPv6 contact.
 */
inline bool ShouldContactKadSourceOverIPv6(const SKadSourceFacts &facts)
{
	const bool firewalledOnIPv4 = facts.sourceType == 3 || facts.sourceType == 5 || facts.sourceType == 6;
	if (!firewalledOnIPv4 || facts.ourHelloID == 0 || !IsLowID(facts.ourHelloID)) {
		return false;
	}
	if (!PeerAddressing::IsUsableTagIdentity(facts.publishedIPv6)) {
		return false;
	}
	SContactFacts contact;
	contact.contactAddress = facts.publishedIPv6;
	contact.ipv6Built = facts.ipv6Built;
	contact.familyPermitted = facts.familyPermitted;
	contact.proxyEnabled = facts.proxyEnabled;
	return DecideContactRoute(contact) == EContactRoute::DialIPv6;
}

#endif // KAD_SOURCE_ADDRESS_POLICY_H
