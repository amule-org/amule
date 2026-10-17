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

#ifndef SERVER_IPV6_SOURCE_POLICY_H
#define SERVER_IPV6_SOURCE_POLICY_H

#include "ContactDialPolicy.h" // Needed for DecideContactRoute
#include "NetworkAddress.h"
#include "PeerAddressing.h" // Needed for IsUsableTagIdentity

struct SServerIPv6SourceFacts
{
	CNetworkAddress address; // The sentinel record's address; Absent when missing or malformed.
	bool ipv6Built = false;
	bool familyPermitted = false;
	bool proxyEnabled = false;
};

//! Whether a sentinel record's IPv6 source is dialled. Unlike a Kad result, the record names no
//! LowID and so offers no server callback to lose: our own ID does not matter here.
inline bool ShouldContactServerSourceOverIPv6(const SServerIPv6SourceFacts &facts)
{
	if (!PeerAddressing::IsUsableTagIdentity(facts.address)) {
		return false;
	}
	SContactFacts contact;
	contact.contactAddress = facts.address;
	contact.ipv6Built = facts.ipv6Built;
	contact.familyPermitted = facts.familyPermitted;
	contact.proxyEnabled = facts.proxyEnabled;
	return DecideContactRoute(contact) == EContactRoute::DialIPv6;
}

#endif // SERVER_IPV6_SOURCE_POLICY_H
