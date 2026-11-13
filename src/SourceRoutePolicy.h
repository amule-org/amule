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

/**
 * What the GUIs and the API are told about how a source is being reached.
 *
 * The first values are ESourceRoute's, one for one. The two NAT-T steps below them are not routes
 * NextSourceRoute() picks: while we are LowID and the source is too, the hole punch and the
 * Lugdunum callback are driven by the NAT-T code, which asks the source's server rather than
 * choosing among this source's own routes. They stay out of ESourceRoute so they never enter the
 * failed-route mask or the chain.
 */
enum class EContactRouteReport : std::uint8_t
{
	None = 0,
	Dial = 1,
	DirectUdpCallback = 2,
	ServerCallback = 3,
	KadBuddy = 4,
	ForeignNatCallback = 5,
	//! Our server punches a hole to the source's server.
	ServerHolePunch = 6,
	//! The source's own server wakes it, on a Lugdunum extension.
	LugdunumCallback = 7,
};

inline EContactRouteReport ReportedRoute(ESourceRoute route)
{
	static_assert(static_cast<unsigned>(EContactRouteReport::Dial) ==
				      static_cast<unsigned>(ESourceRoute::Dial) &&
			      static_cast<unsigned>(EContactRouteReport::ForeignNatCallback) ==
				      static_cast<unsigned>(ESourceRoute::ForeignNatCallback),
		"the reported values must match ESourceRoute one for one");
	return static_cast<EContactRouteReport>(route);
}

//! EC_TAG_CLIENT_CONTACT_ROUTE: the route in the low nibble, its flags above it.
constexpr std::uint8_t ContactRouteMask = 0x0F;
constexpr std::uint8_t ContactRouteUnprovenBit = 0x10;
constexpr std::uint8_t ContactRouteFallbackBit = 0x20;
constexpr std::uint8_t ContactRouteIPv6Bit = 0x40;

inline std::uint8_t PackContactRoute(
	EContactRouteReport route, bool unproven, bool identityFromFallback, bool overIPv6)
{
	static_assert(static_cast<unsigned>(EContactRouteReport::LugdunumCallback) <= ContactRouteMask,
		"every route must fit in the low nibble");
	return static_cast<std::uint8_t>(
		static_cast<std::uint8_t>(route) | (unproven ? ContactRouteUnprovenBit : 0) |
		(identityFromFallback ? ContactRouteFallbackBit : 0) | (overIPv6 ? ContactRouteIPv6Bit : 0));
}

//! None for a route a newer daemon knows and this build does not.
inline EContactRouteReport UnpackContactRoute(std::uint8_t packed)
{
	const std::uint8_t route = packed & ContactRouteMask;
	return route <= static_cast<std::uint8_t>(EContactRouteReport::LugdunumCallback)
		       ? static_cast<EContactRouteReport>(route)
		       : EContactRouteReport::None;
}

inline bool ContactRouteIsUnproven(std::uint8_t packed)
{
	return (packed & ContactRouteUnprovenBit) != 0;
}

inline bool ContactRouteFromFallback(std::uint8_t packed)
{
	return (packed & ContactRouteFallbackBit) != 0;
}

//! The route is a dial to the peer's IPv6 address. A callback route never sets it.
inline bool ContactRouteOverIPv6(std::uint8_t packed)
{
	return (packed & ContactRouteIPv6Bit) != 0;
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
	bool existingHasLastResortIPv4 = false;
	bool existingHasServerCandidate = false;
	// The duplicate: a LowID from a server, a Kad type 6 result, an IPv6 source.
	bool incomingServerLowID = false;
	bool incomingDirectCallback = false;
	bool incomingRoutableIPv6 = false;
	// A HighID with a usable IPv4 address.
	bool incomingHighIDIPv4 = false;
	// The duplicate came from the server we are connected to, which knows the source's current ID.
	bool incomingTrusted = false;
	// The duplicate carries the LowID and server, or the HighID address and port, the kept copy has.
	bool sameServerRoute = false;
	bool sameHighID = false;
};

struct SSourceRouteMerge
{
	// Taken where the kept copy has no server route, and replacing a different one from our server.
	bool takeServerRoute = false;
	// A different server route from anywhere else, tried once every other route has failed.
	bool storeServerCandidate = false;
	// Our server says the source is a HighID at this address now.
	bool replaceWithHighID = false;
	// What a replace from our server overwrites stays as a fallback, so a client that logged in to
	// our server with this source's hash cannot take away a route that works. It takes the slot
	// even when a fallback is stored there: a route that was in use ranks above one that never was.
	bool keepOldHighIDAsLastResort = false;
	bool keepOldServerRouteAsCandidate = false;
	bool takeDirectCallback = false;
	// Dialled before the IPv4 address, as one a peer advertised in its hello.
	bool takeAdvertisedIPv6 = false;
	// Without an IPv4 address an advertised IPv6 is never dialled, so it waits until every other
	// route has failed.
	bool takeLastResortIPv6 = false;
	// An IPv4 the kept copy cannot use while it has routes of its own: the copy moves to it once they
	// have all failed, as if that IPv4 copy had arrived first.
	bool takeLastResortIPv4 = false;
};

/**
 * What a duplicate copy of a known source changes in the copy we keep, beside its Kad buddy
 * (KadBuddyMergePolicy.h).
 *
 * A route the kept copy lacks is taken. A route it has with a different value is replaced only
 * when the duplicate came from the server we are connected to: the server assigned the LowID and
 * tested the HighID. The replaced value stays as a fallback, because the server does not check the
 * hash a client logs in with. From anywhere else a different value is only a fallback, so an old or
 * forged entry cannot replace a route that still works.
 *
 * A kept IPv6-only client takes a server route, which it uses once its IPv6 dial fails. An IPv4
 * address is kept apart as a last resort rather than taken as its identity: a hello over the IPv6
 * would keep that unproven IPv4, and bans and hash tracking would key on it.
 */
inline SSourceRouteMerge DecideSourceRouteMerge(const SSourceRouteMergeFacts &facts)
{
	SSourceRouteMerge merge;
	const bool keptHighID = !facts.existingLowID && !facts.existingNativeIPv6;
	if (facts.incomingServerLowID && !facts.sameServerRoute) {
		if (!keptHighID && !facts.existingHasServer) {
			merge.takeServerRoute = true;
		} else if (facts.incomingTrusted) {
			merge.takeServerRoute = true;
			merge.keepOldHighIDAsLastResort = keptHighID;
			merge.keepOldServerRouteAsCandidate = !keptHighID;
		} else {
			merge.storeServerCandidate = !facts.existingHasServerCandidate;
		}
	}
	if (facts.incomingHighIDIPv4 && !facts.sameHighID) {
		if (facts.incomingTrusted && !facts.existingNativeIPv6) {
			merge.replaceWithHighID = true;
			merge.keepOldHighIDAsLastResort = keptHighID;
			merge.keepOldServerRouteAsCandidate = !keptHighID && facts.existingHasServer;
		} else {
			merge.takeLastResortIPv4 = !facts.existingHasLastResortIPv4;
		}
	}
	if (facts.existingNativeIPv6) {
		merge.takeLastResortIPv4 = merge.takeLastResortIPv4 ||
					   (facts.incomingDirectCallback && !facts.existingHasLastResortIPv4);
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

//! Routes a source keeps for when every route of its own has failed.
enum class ESourceFallback : std::uint8_t
{
	ServerCandidate,
	LastResortIPv4,
	LastResortIPv6,
};

struct SSourceFallbacks
{
	bool serverCandidate = false;
	bool lastResortIPv4 = false;
	bool lastResortIPv6 = false;
	// An IPv6-only client is already on its IPv6.
	bool nativeIPv6 = false;
};

/**
 * Uses the first fallback that leaves the source a route, in the order: server route, IPv4, IPv6.
 * @a tryFallback applies one and tells whether NextSourceRoute() now gives a route. A fallback that
 * gives none is spent and the next one is tried. Returns whether one gave a route.
 */
template <typename TryFallback>
inline bool UseFirstUsableFallback(const SSourceFallbacks &available, TryFallback tryFallback)
{
	if (available.serverCandidate && tryFallback(ESourceFallback::ServerCandidate)) {
		return true;
	}
	if (available.lastResortIPv4 && tryFallback(ESourceFallback::LastResortIPv4)) {
		return true;
	}
	return available.lastResortIPv6 && !available.nativeIPv6 &&
	       tryFallback(ESourceFallback::LastResortIPv6);
}

/**
 * The failed routes still failed on a fallback. A failure on one address or server route says
 * nothing about another, so the routes it changes become untried again.
 */
inline unsigned FailedAfterFallback(ESourceFallback fallback, unsigned failed)
{
	if (fallback == ESourceFallback::ServerCandidate) {
		return failed & ~SourceRouteBit(ESourceRoute::ServerCallback);
	}
	return failed &
	       ~(SourceRouteBit(ESourceRoute::Dial) | SourceRouteBit(ESourceRoute::DirectUdpCallback));
}

#endif // SOURCE_ROUTE_POLICY_H
