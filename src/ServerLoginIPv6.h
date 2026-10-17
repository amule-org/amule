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

#ifndef SERVER_LOGIN_IPV6_H
#define SERVER_LOGIN_IPV6_H

#include "NetworkAddress.h"

#include <optional>

struct SServerLoginIPv6Decision
{
	bool advertise = false;
	CNetworkAddress address; // Present only when advertise is true.
};

//! Whether the login carries CT_MOD_IP_V6 and the CT_SERVER_FLAGS 0x1000 bit. The wire contract
//! requires both or neither: a server that sees the tag sends sentinel source records.
inline SServerLoginIPv6Decision DecideServerLoginIPv6(const std::optional<CNetworkAddress> &advertisedIPv6)
{
	SServerLoginIPv6Decision decision;
	if (advertisedIPv6) {
		decision.advertise = true;
		decision.address = *advertisedIPv6;
	}
	return decision;
}

#endif // SERVER_LOGIN_IPV6_H
