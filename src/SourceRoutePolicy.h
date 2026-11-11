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

#ifndef SOURCE_ROUTE_POLICY_H
#define SOURCE_ROUTE_POLICY_H

#include <cstdint>

//! The ways a download source is contacted. An advertised IPv6 is not one of them: an unanswered
//! one is remembered by its address and the next dial falls back to IPv4 (ContactDialPolicy.h).
enum class ESourceRoute : std::uint8_t
{
	None,
	Dial,
	DirectUdpCallback,
	ServerCallback,
	// A callback through the Kad buddy, or the search for its address.
	KadBuddy,
	ForeignNatCallback,
};

inline unsigned SourceRouteBit(ESourceRoute route)
{
	return 1u << static_cast<unsigned>(route);
}

inline const char *SourceRouteName(ESourceRoute route)
{
	switch (route) {
	case ESourceRoute::Dial:
		return "dial";
	case ESourceRoute::DirectUdpCallback:
		return "direct UDP callback";
	case ESourceRoute::ServerCallback:
		return "server callback";
	case ESourceRoute::KadBuddy:
		return "Kad buddy";
	case ESourceRoute::ForeignNatCallback:
		return "foreign-server NAT callback";
	case ESourceRoute::None:
		break;
	}
	return "none";
}

struct SSourceRouteFacts
{
	// A LowID that is not dialled over IPv6, so only a callback reaches it.
	bool callbackOnly = false;
	bool directCallback = false;
	bool onOurServer = false;
	bool hasBuddyID = false;
	bool kadConnected = false;
	// We can be called back, which a HighID's buddy route needs. A LowID route checked it already.
	bool canTakeCallback = false;
	bool foreignNatCallback = false;
	// SourceRouteBit() of each route that failed since the last completed hello.
	unsigned failed = 0;
};

/**
 * The next route to contact a source on, or None when every route it offers has failed.
 *
 * The order is the one eMule has always used, and a failed route moves on to the next instead of
 * giving the source up. A HighID's buddy is a Kad result newer than the HighID: the source has
 * become firewalled since, so its buddy is tried once the dial fails. A LowID reached by its IPv6
 * moves on to its callback routes once that dial fails: the caller passes callbackOnly then.
 */
inline ESourceRoute NextSourceRoute(const SSourceRouteFacts &facts)
{
	const auto untried = [&facts](ESourceRoute route) {
		return (facts.failed & SourceRouteBit(route)) == 0;
	};
	if (facts.directCallback && untried(ESourceRoute::DirectUdpCallback)) {
		return ESourceRoute::DirectUdpCallback;
	}
	const bool buddy = facts.hasBuddyID && facts.kadConnected && untried(ESourceRoute::KadBuddy);
	if (facts.callbackOnly) {
		if (facts.onOurServer && untried(ESourceRoute::ServerCallback)) {
			return ESourceRoute::ServerCallback;
		}
		if (buddy) {
			return ESourceRoute::KadBuddy;
		}
		if (facts.foreignNatCallback && untried(ESourceRoute::ForeignNatCallback)) {
			return ESourceRoute::ForeignNatCallback;
		}
		return ESourceRoute::None;
	}
	if (untried(ESourceRoute::Dial)) {
		return ESourceRoute::Dial;
	}
	return buddy && facts.canTakeCallback ? ESourceRoute::KadBuddy : ESourceRoute::None;
}

struct SSourceRouteMergeFacts
{
	// The copy we keep: its user address is a native IPv6, its ID is Kad's placeholder 1.
	bool existingNativeIPv6 = false;
	bool existingKadPlaceholderID = false;
	bool existingHasServer = false;
	bool existingLowID = false;
	bool existingDirectCallback = false;
	bool existingHasIPv4 = false;
	bool sameIPv4 = false;
	bool existingHasAdvertisedIPv6 = false;
	bool existingHasLastResortIPv6 = false;
	// The duplicate: a LowID from a server, a Kad type 6 result, an IPv6 source.
	bool incomingServerLowID = false;
	bool incomingDirectCallback = false;
	bool incomingRoutableIPv6 = false;
};

struct SSourceRouteMerge
{
	bool takeServerRoute = false;
	bool takeDirectCallback = false;
	// Dialled before the IPv4 address, as one a peer advertised in its hello.
	bool takeAdvertisedIPv6 = false;
	// Without an IPv4 address an advertised IPv6 is never dialled, so it waits until every other
	// route has failed.
	bool takeLastResortIPv6 = false;
};

/**
 * Which contact routes a duplicate copy of a known source hands to the copy we keep, beside its
 * Kad buddy (KadBuddyMergePolicy.h). Only what the kept copy lacks is taken, and nothing that
 * contradicts it: a different IPv4 address or server stays with the copy we keep.
 *
 * A kept IPv6-only client takes a server route, which it uses once its IPv6 dial fails, but no
 * IPv4 address: its IPv6 is its identity until a hello over IPv4 moves it.
 */
inline SSourceRouteMerge DecideSourceRouteMerge(const SSourceRouteMergeFacts &facts)
{
	SSourceRouteMerge merge;
	merge.takeServerRoute =
		facts.incomingServerLowID && facts.existingKadPlaceholderID && !facts.existingHasServer;
	if (facts.existingNativeIPv6) {
		return merge;
	}
	merge.takeDirectCallback = facts.incomingDirectCallback && facts.existingLowID &&
				   !facts.existingDirectCallback &&
				   (!facts.existingHasIPv4 || facts.sameIPv4);
	if (facts.incomingRoutableIPv6 && !facts.existingHasAdvertisedIPv6) {
		merge.takeAdvertisedIPv6 = facts.existingHasIPv4 || merge.takeDirectCallback;
		merge.takeLastResortIPv6 = !merge.takeAdvertisedIPv6 && !facts.existingHasLastResortIPv6;
	}
	return merge;
}

#endif // SOURCE_ROUTE_POLICY_H
