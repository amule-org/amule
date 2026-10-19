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

#ifndef SERVER_IPV6_UPDATE_H
#define SERVER_IPV6_UPDATE_H

#include "NetworkAddress.h"
#include <protocol/ed2k/Client2Server/TCP.h> // Needed for SRV_TCPFLG_IPV6*

#include <cstdint>
#include <cstring>
#include <optional>

//! 0x8000 alone also reads as a NAT rendezvous flag elsewhere, so it counts only from a server that
//! speaks the IPv6 extension (0x4000).
inline bool ServerAcceptsIPv6Updates(std::uint32_t tcpFlags)
{
	const std::uint32_t required = SRV_TCPFLG_IPV6 | SRV_TCPFLG_IPV6_UPDATE;
	return (tcpFlags & required) == required;
}

struct SServerIPv6UpdateDecision
{
	bool send = false;
	std::uint8_t payload[16] = {}; // All zeros withdraws; meaningful only when send.
};

//! Whether to send OP_IPV6_UPDATE on this server connection, and its 16-byte payload.
//!
//! @param serverAcceptsUpdates ServerAcceptsIPv6Updates() for this connection's OP_IDCHANGE flags.
//! @param lastSent             The address this connection last sent (by login or a prior
//!                             update); Absent if nothing was sent yet.
//! @param advertised           The current AdvertisedIPv6ForPeers() result.
inline SServerIPv6UpdateDecision DecideServerIPv6Update(bool serverAcceptsUpdates,
	const std::optional<CNetworkAddress> &lastSent,
	const std::optional<CNetworkAddress> &advertised)
{
	SServerIPv6UpdateDecision decision;
	if (!serverAcceptsUpdates || advertised == lastSent) {
		return decision;
	}
	decision.send = true;
	if (advertised) {
		advertised->ToIPv6Bytes(decision.payload);
	}
	return decision;
}

#endif // SERVER_IPV6_UPDATE_H
