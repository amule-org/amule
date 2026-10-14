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

#include <vector>

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
 * The addresses an outbound contact must pass the IP filter and ban list on: the dial target and,
 * for a promoted IPv6 dial, the peer's retained IPv4 identity, so promotion never bypasses a
 * filter or ban on either. Both are the same address on every other route.
 */
inline std::vector<CNetworkAddress> ContactSecurityAddresses(
	const CNetworkAddress &target, const CNetworkAddress &connectAddress)
{
	std::vector<CNetworkAddress> addresses{ target };
	if (PeerAddressing::HasEd2kWireForm(connectAddress) && connectAddress != target) {
		addresses.push_back(connectAddress);
	}
	return addresses;
}

/** Whether dialling dialAddress uses a peer's advertised IPv6 in place of its IPv4 identity. */
inline bool IsPromotedIPv6Dial(const CNetworkAddress &dialAddress, const CNetworkAddress &connectAddress)
{
	return dialAddress.IsIPv6() && !dialAddress.IsIPv4Mapped() &&
	       PeerAddressing::HasEd2kWireForm(connectAddress);
}

/** uTP framing is IPv4-only, so an IPv6 dial target goes over TCP. */
inline bool ShouldTryIPv4Utp(const CNetworkAddress &dialAddress)
{
	return !(dialAddress.IsIPv6() && !dialAddress.IsIPv4Mapped());
}

/**
 * Returns a peer's advertised IPv6 as the TCP target only when this node is explicitly configured
 * for dual stack and has selected a global local IPv6. The local address proves that an IPv6 source
 * route exists; it is not the peer target. Otherwise the existing target stays intact.
 */
inline CNetworkAddress PromotePeerIPv6ForDial(const CNetworkAddress &current,
	const CNetworkAddress &peerIPv6,
	const CNetworkAddress &unansweredIPv6,
	bool dualStackConfigured,
	const CNetworkAddress &localIPv6,
	bool proxyEnabled)
{
	// Only add IPv6 as an alternative to an established IPv4 identity. Existing native IPv6
	// contacts must keep the exact endpoint they already use, and an unanswered advertisement
	// falls back to that IPv4 route rather than being retried forever.
	if (PeerAddressing::HasEd2kWireForm(current) && dualStackConfigured && !proxyEnabled &&
		peerIPv6 != unansweredIPv6 && peerIPv6.IsGloballyRoutableIPv6() &&
		localIPv6.IsGloballyRoutableIPv6()) {
		return peerIPv6;
	}
	return current;
}

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
