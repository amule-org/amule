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

#ifndef STORED_SOURCE_ROUTE_H
#define STORED_SOURCE_ROUTE_H

#include "DeadSourceKey.h"
#include "NetworkAddress.h"
#include "NetworkFunctions.h" // Needed for IsLowID

#include <cstdint>
#include <vector>

//! A way to reach a download source that its client does not use yet: a fallback server route, or
//! a last-resort address. A zero field leaves the client's own value when the route is applied.
struct SStoredSourceRoute
{
	std::uint32_t id = 0;
	CNetworkAddress address;
	std::uint32_t serverIP = 0;
	std::uint16_t serverPort = 0;
	std::uint16_t tcpPort = 0;
	std::uint16_t kadPort = 0;
	bool directCallback = false;

	bool Present() const { return id != 0 || address.IsPresent(); }

	//! The key CDeadSourceList builds for a client on this route.
	CDeadSourceKey DeadKey() const
	{
		return CDeadSourceKey(CProtocolPeerIdentity::FromClientState(
					      id, IsLowID(id), address, serverIP, serverPort),
			id,
			tcpPort,
			serverIP,
			kadPort);
	}
};

/**
 * Every key a dead source is recorded under: its current route, each route it could still have
 * used, and the routes it already left. A later copy under any of them is then refused.
 */
inline std::vector<CDeadSourceKey> DeadSourceKeysFor(const SStoredSourceRoute &current,
	const std::vector<SStoredSourceRoute> &others,
	const std::vector<CDeadSourceKey> &former)
{
	std::vector<CDeadSourceKey> keys{ current.DeadKey() };
	for (const SStoredSourceRoute &route : others) {
		if (route.Present()) {
			keys.push_back(route.DeadKey());
		}
	}
	keys.insert(keys.end(), former.begin(), former.end());
	return keys;
}

#endif // STORED_SOURCE_ROUTE_H
