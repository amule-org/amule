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

#ifndef ADVERTISED_IPV6_H
#define ADVERTISED_IPV6_H

#include "NetworkAddress.h"
#include "NetworkInterfaces.h"
#include "PeerAddressing.h" // Needed for IsUsableTagIdentity

#include <optional>
#include <tuple>
#include <vector>

/**
 * The IPv6 address to give peers as ours, or none.
 *
 * Peers dial whatever is advertised, so a candidate must be an address our own tag parser would
 * accept, and never a deprecated one, which is kept for existing connections only. With an
 * interface pin, inbound traffic arrives only through that interface, so only its addresses count.
 * A stable address is preferred over an RFC 4941 temporary one, which the system rotates away from
 * peers that stored it. The lowest address wins a tie, so every refresh agrees whatever order the
 * platform enumerates in.
 */
inline std::optional<CNetworkAddress> ChooseAdvertisedIPv6(
	const std::vector<NetworkInterface> &interfaces, const wxString &pinnedInterface)
{
	const NetworkInterfaceIPv6 *best = nullptr;
	for (const NetworkInterface &iface : interfaces) {
		if (!pinnedInterface.IsEmpty() && iface.name != pinnedInterface) {
			continue;
		}
		for (const NetworkInterfaceIPv6 &address : iface.ipv6) {
			if (address.deprecated || !PeerAddressing::IsUsableTagIdentity(
							  CNetworkAddress::IPv6FromOctets(address.bytes))) {
				continue;
			}
			if (!best || std::tie(address.temporary, address.bytes) <
					     std::tie(best->temporary, best->bytes)) {
				best = &address;
			}
		}
	}
	if (!best) {
		return std::nullopt;
	}
	return CNetworkAddress::IPv6FromOctets(best->bytes);
}

struct SAdvertisedIPv6Facts
{
	std::optional<CNetworkAddress> choice; // ChooseAdvertisedIPv6() at the last interface refresh
	bool contradicted = false;             // Peers see us arrive from a /64 we do not hold
	bool declaredReachable = false;        // The user says the ed2k TCP port accepts inbound IPv6
	bool ipv6Built = false;
	bool familyPermitted = false;
	bool proxyEnabled = false;
	bool listenerAcceptsIPv6 = false;
};

/**
 * The IPv6 address to advertise as ours, or none.
 *
 * eMuleAI treats a peer's CT_MOD_IP_V6 as reachable: it dials that address for every later
 * connection with no IPv4 fallback, skips LowID callbacks, keeps it for the whole session and passes
 * it on in source exchange. An address a firewall drops would therefore cost connections we have
 * today, so it is advertised only when the user declares the port reachable, which nothing here can
 * test from outside. A proxy carries IPv4 only and advertising around it would leak our address;
 * without an IPv6 listener there is nothing to reach.
 *
 * Peer corroboration is a veto, not a precondition. Requiring it first deadlocks: votes arrive only
 * over IPv6 connections, which peers open only to an address we advertise. Behind IPv6 NAT, the case
 * it guards against, peers report a /64 we do not hold, and that contradiction withdraws the address.
 */
inline std::optional<CNetworkAddress> DecideAdvertisedIPv6(const SAdvertisedIPv6Facts &facts)
{
	const bool reachable = facts.declaredReachable && !facts.contradicted;
	const bool permitted = facts.ipv6Built && facts.familyPermitted && !facts.proxyEnabled;
	if (!facts.choice || !reachable || !permitted || !facts.listenerAcceptsIPv6) {
		return std::nullopt;
	}
	return facts.choice;
}

/**
 * Whether Kad source records need a prompt republish after the IPv6 advertising state changes.
 *
 * Every reason to stop advertising now makes the address in existing records wrong: the
 * configuration or local address changed, or peers contradict it. So any change to what would be
 * published, including withdrawing it, republishes.
 */
inline bool ShouldRepublishKadSources(
	const std::optional<CNetworkAddress> &lastPublished, const SAdvertisedIPv6Facts &facts)
{
	return DecideAdvertisedIPv6(facts) != lastPublished;
}

#ifdef ENABLE_IPV6
/**
 * The IPv6 address our hellos carry right now, or none.
 *
 * Defined in amule.cpp, beside the interface refresh that feeds it, so this header stays clear of
 * the app and its sockets and the decision above can be unit-tested on its own.
 */
std::optional<CNetworkAddress> AdvertisedIPv6ForPeers();

/**
 * The stable globally routable local IPv6 selected from the current interfaces, or none.
 *
 * Unlike AdvertisedIPv6ForPeers(), this does not assert inbound reachability or opt the address
 * into hello advertisement. It is only evidence that an outbound IPv6 source route exists.
 */
std::optional<CNetworkAddress> SelectedLocalIPv6();
#endif

#endif // ADVERTISED_IPV6_H
