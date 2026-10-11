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

#endif // ADVERTISED_IPV6_H
