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

#ifndef CONTACT_DIAL_POLICY_H
#define CONTACT_DIAL_POLICY_H

#include "NetworkAddress.h"
#include "PeerAddressing.h" // Needed for HasEd2kWireForm

enum class EContactRoute
{
	Legacy,
	DialIPv6,
	Refuse
};

struct SContactFacts
{
	CNetworkAddress contactAddress;
	bool socketConnected = false;
	bool ipv6Built = false;
	bool familyPermitted = false;
	bool proxyEnabled = false;
};

/**
 * How to reach a client.
 *
 * Legacy is the path an IPv4 or absent contact has always taken: the connected socket, an IPv4
 * dial, or a callback for a LowID. A native IPv6 contact is dialled directly even though its zero
 * hybrid ID makes it a LowID, because every callback route carries IPv4 only. It is refused, as
 * before, without IPv6 support or permission, or with a proxy configured: the proxy protocols
 * here carry IPv4 only, and dialling around the proxy would bypass it.
 */
inline EContactRoute DecideContactRoute(const SContactFacts &facts)
{
	const CNetworkAddress &address = facts.contactAddress;
	if (facts.socketConnected || address.IsAbsent() || PeerAddressing::HasEd2kWireForm(address)) {
		return EContactRoute::Legacy;
	}
	const bool nativeIPv6 = address.IsIPv6() && !address.IsIPv4Mapped() && !address.IsUnspecified();
	if (nativeIPv6 && facts.ipv6Built && facts.familyPermitted && !facts.proxyEnabled) {
		return EContactRoute::DialIPv6;
	}
	return EContactRoute::Refuse;
}

#endif // CONTACT_DIAL_POLICY_H
