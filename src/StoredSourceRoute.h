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

#include <cstddef>
#include <cstdint>
#include <optional>
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

	bool operator==(const SStoredSourceRoute &other) const
	{
		return id == other.id && address == other.address && serverIP == other.serverIP &&
		       serverPort == other.serverPort && tcpPort == other.tcpPort &&
		       kadPort == other.kadPort && directCallback == other.directCallback;
	}

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

//! The routes of a client that a route chain changes.
struct SSourceRoutes
{
	SStoredSourceRoute route;
	SStoredSourceRoute serverCandidate;
	SStoredSourceRoute lastResortIPv4;
	SStoredSourceRoute lastResortIPv6;
	bool identityFromFallback = false;
};

/**
 * The routes a client had when its route chain started. A client that survives a chain with no
 * working route gets them back, so its next chain starts from its own route again.
 */
class CSourceRouteChainStart
{
public:
	struct SStart
	{
		SSourceRoutes routes;
		//! How many former dead-source keys the client had.
		std::size_t formerKeys = 0;
	};

	//! Keeps the routes of the first change in a chain; later calls change nothing.
	void Save(const SSourceRoutes &routes, std::size_t formerKeys)
	{
		if (!m_start) {
			m_start = SStart{ routes, formerKeys };
		}
	}

	//! A source list changed the routes from before to after during the chain. The saved start
	//! takes each changed value, so the restore does not undo it.
	void Carry(const SSourceRoutes &before, const SSourceRoutes &after)
	{
		if (!m_start) {
			return;
		}
		SSourceRoutes &saved = m_start->routes;
		CarryValue(before.route.id, after.route.id, saved.route.id);
		CarryValue(before.route.address, after.route.address, saved.route.address);
		CarryValue(before.route.serverIP, after.route.serverIP, saved.route.serverIP);
		CarryValue(before.route.serverPort, after.route.serverPort, saved.route.serverPort);
		CarryValue(before.route.tcpPort, after.route.tcpPort, saved.route.tcpPort);
		CarryValue(before.route.kadPort, after.route.kadPort, saved.route.kadPort);
		CarryValue(before.route.directCallback, after.route.directCallback, saved.route.directCallback);
		CarryValue(before.serverCandidate, after.serverCandidate, saved.serverCandidate);
		CarryValue(before.lastResortIPv4, after.lastResortIPv4, saved.lastResortIPv4);
		CarryValue(before.lastResortIPv6, after.lastResortIPv6, saved.lastResortIPv6);
		CarryValue(before.identityFromFallback, after.identityFromFallback, saved.identityFromFallback);
	}

	//! The saved start, which ends the chain.
	std::optional<SStart> Take()
	{
		std::optional<SStart> start = m_start;
		m_start.reset();
		return start;
	}

	void Reset() { m_start.reset(); }

private:
	template <typename T>
	static void CarryValue(const T &before, const T &after, T &saved)
	{
		if (!(after == before)) {
			saved = after;
		}
	}

	std::optional<SStart> m_start;
};

#endif // STORED_SOURCE_ROUTE_H
