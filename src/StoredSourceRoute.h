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
#include "NetworkFunctions.h" // Needed for IsLowID and IsHighIDOf
#include "SourceRoutePolicy.h"

#include <algorithm>
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
 * The keys a dead source is recorded under: its last route, and the other routes it was seen to fail
 * on, including the ones it left. A later copy under any of them is then refused. An untried route
 * is not one of them: it may name another client now. Nor is a route where another host answered:
 * that host is alive and is not this source.
 */
inline std::vector<CDeadSourceKey> DeadSourceKeysFor(const SStoredSourceRoute &current,
	const std::vector<SStoredSourceRoute> &failed,
	const std::vector<CDeadSourceKey> &former,
	const std::vector<CDeadSourceKey> &otherHosts = {})
{
	std::vector<CDeadSourceKey> keys{ current.DeadKey() };
	for (const SStoredSourceRoute &route : failed) {
		if (route.Present()) {
			keys.push_back(route.DeadKey());
		}
	}
	keys.insert(keys.end(), former.begin(), former.end());
	keys.erase(std::remove_if(keys.begin(),
			   keys.end(),
			   [&otherHosts](const CDeadSourceKey &key) {
				   return std::any_of(otherHosts.begin(),
					   otherHosts.end(),
					   [&key](const CDeadSourceKey &host) { return key.Matches(host); });
			   }),
		keys.end());
	return keys;
}

//! Adds a key where another host answered, once: the same host can answer again.
inline void AddOtherHostKey(std::vector<CDeadSourceKey> &keys, const CDeadSourceKey &key)
{
	if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
		keys.push_back(key);
	}
}

//! Removes that key once the client's own hash answers there again.
inline void RemoveOtherHostKey(std::vector<CDeadSourceKey> &keys, const CDeadSourceKey &key)
{
	keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
}

//! The routes of a client that a route chain changes.
struct SSourceRoutes
{
	SStoredSourceRoute route;
	SStoredSourceRoute serverCandidate;
	SStoredSourceRoute lastResortIPv4;
	SStoredSourceRoute lastResortIPv6;
	bool identityFromFallback = false;
	// The route in use came from a record no hello has confirmed: a fallback, or a server route or
	// direct callback taken from a copy that our server did not send.
	bool routeUnproven = false;
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

	//! The saved routes, while a chain runs.
	std::optional<SSourceRoutes> Routes() const
	{
		return m_start ? std::optional<SSourceRoutes>(m_start->routes) : std::nullopt;
	}

	//! A source list changed the saved routes during the chain: the restore gives back these.
	void Update(const SSourceRoutes &routes)
	{
		if (m_start) {
			m_start->routes = routes;
		}
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
	std::optional<SStart> m_start;
};

//! Adds the facts about the kept copy's routes, and how they compare with the duplicate's route, to
//! the facts the caller knows about the duplicate and the client.
inline SSourceRouteMergeFacts WithRouteFacts(
	SSourceRouteMergeFacts facts, const SSourceRoutes &existing, const SStoredSourceRoute &incoming)
{
	const SStoredSourceRoute &route = existing.route;
	const std::uint32_t ipv4 = route.address.ToIPv4NetworkOrderOrZero();
	const std::uint32_t incomingIPv4 = incoming.address.ToIPv4NetworkOrderOrZero();
	facts.existingNativeIPv6 = route.address.IsIPv6() && !route.address.IsIPv4Mapped();
	facts.existingKadPlaceholderID = route.id == 1;
	facts.existingHasServer = route.serverIP != 0 && route.serverPort != 0;
	facts.existingLowID = IsLowID(route.id);
	facts.existingDirectCallback = route.directCallback;
	facts.existingHasIPv4 = ipv4 != 0;
	facts.sameIPv4 = ipv4 == incomingIPv4;
	facts.existingHasLastResortIPv4 = existing.lastResortIPv4.Present();
	facts.existingHasLastResortIPv6 = existing.lastResortIPv6.Present();
	facts.existingHasServerCandidate = existing.serverCandidate.Present();
	facts.sameServerRoute = route.id == incoming.id && route.serverIP == incoming.serverIP &&
				route.serverPort == incoming.serverPort;
	facts.sameHighID = !IsLowID(route.id) && ipv4 == incomingIPv4 && route.tcpPort == incoming.tcpPort;
	return facts;
}

//! The server route part of a route: its ID, server and port.
inline SStoredSourceRoute ServerRouteOf(const SStoredSourceRoute &route)
{
	SStoredSourceRoute server;
	server.id = route.id;
	server.serverIP = route.serverIP;
	server.serverPort = route.serverPort;
	server.tcpPort = route.tcpPort;
	return server;
}

//! Writes what a merge takes from the duplicate's route into the kept copy's routes. An advertised
//! IPv6 is the caller's: it is not one of these routes.
inline void ApplySourceRouteMerge(
	SSourceRoutes &routes, const SSourceRouteMerge &merge, const SStoredSourceRoute &incoming)
{
	SStoredSourceRoute &route = routes.route;
	if (merge.keepOldHighIDAsLastResort) {
		routes.lastResortIPv4 = route;
	}
	if (merge.keepOldServerRouteAsCandidate) {
		routes.serverCandidate = ServerRouteOf(route);
	}
	if (merge.takeServerRoute) {
		route.id = incoming.id;
		route.serverIP = incoming.serverIP;
		route.serverPort = incoming.serverPort;
	}
	if (merge.storeServerCandidate) {
		routes.serverCandidate = ServerRouteOf(incoming);
	}
	if (merge.replaceWithHighID) {
		route.id = incoming.id;
		route.address = incoming.address;
		route.tcpPort = incoming.tcpPort;
		// Our server tested this address.
		routes.identityFromFallback = false;
	}
	if (merge.takeLastResortIPv4) {
		routes.lastResortIPv4 = incoming;
	}
	if (merge.takeDirectCallback) {
		route.address = CNetworkAddress::FromIPv4NetworkOrderOrAbsent(
			incoming.address.ToIPv4NetworkOrderOrZero());
		route.kadPort = incoming.kadPort;
		route.directCallback = true;
	}
	if (merge.takeLastResortIPv6 && !merge.takeAdvertisedIPv6) {
		routes.lastResortIPv6 = incoming;
	}
}

//! Whether @a id is the HighID of an IPv4 the source is known at: its own or its last-resort IPv4.
inline bool IsHighIDOfKnownIPv4(const SSourceRoutes &routes, std::uint32_t id)
{
	return IsHighIDOf(id, routes.route.address.ToIPv4NetworkOrderOrZero()) ||
	       IsHighIDOf(id, routes.lastResortIPv4.address.ToIPv4NetworkOrderOrZero());
}

/**
 * Merges a duplicate's route into the kept copy's routes: decides with WithRouteFacts(), writes with
 * ApplySourceRouteMerge(), and returns the decision.
 *
 * A server or old source-exchange record gives a HighID as its IP, so a HighID whose IP ends in .0
 * reads as a LowID. A record whose ID is an IPv4 the source is known at is that HighID, on whatever
 * port it lists: the duplicate matched this source already, and a real LowID equals that IPv4 only
 * by chance. @a chainStart is where a running chain started, when it moved the source to a fallback
 * at another address.
 */
inline SSourceRouteMerge MergeSourceRoutes(SSourceRoutes &routes,
	SSourceRouteMergeFacts facts,
	SStoredSourceRoute incoming,
	const SSourceRoutes *chainStart = nullptr)
{
	if (facts.incomingServerLowID &&
		(IsHighIDOfKnownIPv4(routes, incoming.id) ||
			(chainStart && IsHighIDOfKnownIPv4(*chainStart, incoming.id)))) {
		facts.incomingServerLowID = false;
		facts.incomingHighIDIPv4 = true;
		incoming.address = CNetworkAddress::FromIPv4NetworkOrder(incoming.id);
		incoming.id = CNetworkAddress::SwapOctets(incoming.id);
	}
	const SSourceRouteMerge merge = DecideSourceRouteMerge(WithRouteFacts(facts, routes, incoming));
	ApplySourceRouteMerge(routes, merge, incoming);
	if (!facts.incomingTrusted && (merge.takeServerRoute || merge.takeDirectCallback)) {
		routes.routeUnproven = true;
	}
	return merge;
}

#endif // STORED_SOURCE_ROUTE_H
