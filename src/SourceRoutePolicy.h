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

#include "Constants.h" // Needed for the download and upload states
#include <cstdint>
#include <optional>

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
//! An older daemon never sets it: a GUI then reads every attempt as live, as before.
constexpr std::uint8_t ContactRouteEndedBit = 0x80;

inline std::uint8_t PackContactRoute(
	EContactRouteReport route, bool unproven, bool identityFromFallback, bool overIPv6, bool ended)
{
	static_assert(static_cast<unsigned>(EContactRouteReport::LugdunumCallback) <= ContactRouteMask,
		"every route must fit in the low nibble");
	return static_cast<std::uint8_t>(
		static_cast<std::uint8_t>(route) | (unproven ? ContactRouteUnprovenBit : 0) |
		(identityFromFallback ? ContactRouteFallbackBit : 0) | (overIPv6 ? ContactRouteIPv6Bit : 0) |
		(ended ? ContactRouteEndedBit : 0));
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

//! The attempt is no longer awaited (ContactAttemptAwaited()).
inline bool ContactRouteEnded(std::uint8_t packed)
{
	return (packed & ContactRouteEndedBit) != 0;
}

/**
 * How the connection a client is on, or was last on, came about. Unlike EContactRouteReport, which
 * names the attempt, this is decided once per connection, from its first hello.
 */
enum class EConnectionRoute : std::uint8_t
{
	None = 0,
	//! We dialled the peer.
	Dial = 1,
	//! The peer dialled us, not in answer to anything we asked.
	Inbound = 2,
	//! The peer dialled us in answer to our callback request through our server.
	ServerCallback = 3,
	KadBuddyCallback = 4,
	DirectUdpCallback = 5,
	ForeignNatCallback = 6,
	ServerHolePunch = 7,
	LugdunumCallback = 8,
	//! We dialled the peer because our server asked us to.
	AnsweredServerCallback = 9,
	//! We dialled the peer because its request reached us through our Kad buddy.
	AnsweredKadCallback = 10,
	AnsweredDirectCallback = 11,
	//! We dialled the peer because our server forwarded its Lugdunum callback request.
	AnsweredLugdunumCallback = 12,
	//! We dialled the peer after a rendezvous we asked its Kad buddy for.
	KadBuddyRendezvous = 13,
	//! The peer dialled us after our Kad buddy forwarded its rendezvous.
	AnsweredKadBuddyRendezvous = 14,
};

enum class EConnectionTransport : std::uint8_t
{
	TCP = 0,
	UTP = 1,
	QUIC = 2,
};

//! Why a client socket was opened: by us, for one of these, or accepted.
enum class EConnectionOpener : std::uint8_t
{
	Accepted,
	Dial,
	AnswerServerCallback,
	AnswerKadCallback,
	AnswerDirectCallback,
	HolePunchDial,
	LugdunumCallbackDial,
	AnswerLugdunumCallback,
	//! Accepted from a peer a server punch announced.
	AcceptedRendezvous,
	//! Accepted from a peer a Lugdunum callback announced.
	AcceptedLugdunumRendezvous,
	//! A hole-punch dial on a rendezvous we asked a Kad source's buddy for.
	KadRendezvousDial,
	//! Accepted from a peer whose rendezvous our Kad buddy forwarded.
	AcceptedKadRendezvous,
};

/**
 * Why a socket a client opens now is opened: a callback request it answers, else what the NAT-T
 * dial flags of a temporary client say, else our own dial. @a kadRendezvousDial counts only on a
 * hole-punch dial.
 */
inline EConnectionOpener ResolveConnectionOpener(
	EConnectionOpener answer, bool lugdunumCallbackDial, bool holePunchDial, bool kadRendezvousDial)
{
	if (answer != EConnectionOpener::Dial) {
		return answer;
	}
	if (lugdunumCallbackDial) {
		return EConnectionOpener::LugdunumCallbackDial;
	}
	if (!holePunchDial) {
		return EConnectionOpener::Dial;
	}
	return kadRendezvousDial ? EConnectionOpener::KadRendezvousDial : EConnectionOpener::HolePunchDial;
}

//! How long a NAT-T step of ours explains a new connection: the window its rendezvous has.
constexpr std::uint64_t kNatStepAnswerWindowMs = 60'000;
//! How long our Kad rendezvous request for a source lasts: NatRendezvous::kRequesterAttemptMs.
constexpr std::uint64_t kKadRendezvousAttemptMs = 45'000;

//! The NAT-T steps that ask a server while both ends are LowID.
inline bool IsServerNatStep(EContactRouteReport route)
{
	return route == EContactRouteReport::ForeignNatCallback ||
	       route == EContactRouteReport::ServerHolePunch ||
	       route == EContactRouteReport::LugdunumCallback;
}

struct SKadRendezvousRetryFacts
{
	//! The source's last contact step. A step that ended leaves its route behind.
	EContactRouteReport lastRoute = EContactRouteReport::None;
	//! Our OP_CALLBACKREQUEST or OP_IPV6_CALLBACK_REQUEST for it waits for its answer.
	bool serverCallbackPending = false;
	//! A 0x60 for it is allowed but has not gone out: the request limits held it back.
	bool holePunchOwed = false;
	//! The last NAT-T server step can no longer be answered: its answer window passed, or the
	//! server refused the punch.
	bool natStepOver = false;
};

//! Whether a source left waiting LowID-to-LowID may be asked for the Kad rendezvous again: only
//! while no server path works on it, or a second path would take its punch and dial it twice.
inline bool MayRetryKadRendezvous(const SKadRendezvousRetryFacts &facts)
{
	if (facts.holePunchOwed) {
		return false;
	}
	switch (facts.lastRoute) {
	case EContactRouteReport::None:
	case EContactRouteReport::KadBuddy:
		return true;
	case EContactRouteReport::ServerCallback:
		return !facts.serverCallbackPending;
	case EContactRouteReport::ForeignNatCallback:
	case EContactRouteReport::ServerHolePunch:
	case EContactRouteReport::LugdunumCallback:
		return facts.natStepOver;
	default:
		return false;
	}
}

struct SContactInFlightFacts
{
	std::uint8_t downloadState = DS_NONE;
	std::uint8_t uploadState = US_NONE;
	//! The route of our last download contact, and how long ago its NAT-T step went out.
	EContactRouteReport attempt = EContactRouteReport::None;
	std::uint64_t natStepAgeMs = 0;
	//! An upload callback of ours waits for its answer, and the route it went out on.
	bool uploadCallbackInFlight = false;
	EContactRouteReport uploadCallbackRoute = EContactRouteReport::None;
};

/**
 * The contact of ours a new connection may answer: its route, or none when nothing of ours is
 * awaited. DS_LOWTOLOWIP counts only with a NAT-T step still in its answer window: the state is
 * also set when no step could be sent, and it lasts as long as we stay LowID.
 */
inline std::optional<EContactRouteReport> ContactInFlight(const SContactInFlightFacts &facts)
{
	switch (facts.downloadState) {
	case DS_CONNECTING:
	case DS_WAITCALLBACK:
	case DS_WAITCALLBACKKAD:
		return facts.attempt;
	case DS_LOWTOLOWIP:
		// Not a Kad rendezvous: only our own dial answers it, and its opener says so.
		if (IsServerNatStep(facts.attempt) && facts.natStepAgeMs < kNatStepAnswerWindowMs) {
			return facts.attempt;
		}
		break;
	default:
		break;
	}
	if (facts.uploadState == US_CONNECTING) {
		return facts.attempt;
	}
	if (facts.uploadCallbackInFlight) {
		return facts.uploadCallbackRoute;
	}
	return std::nullopt;
}

struct SContactAttemptFacts
{
	std::uint8_t downloadState = DS_NONE;
	std::uint8_t uploadState = US_NONE;
	EContactRouteReport attempt = EContactRouteReport::None;
	//! How long ago the last NAT-T step went out, and whether the server refused its punch.
	std::uint64_t stepAgeMs = 0;
	bool punchRefused = false;
};

/**
 * Whether the attempt EContactRouteReport names is still awaited, which the GUIs say as
 * "Connecting via". DS_LOWTOLOWIP can outlast its step by hours: a server step is awaited for its
 * answer window unless its punch was refused, and a Kad rendezvous while its attempt lasts.
 */
inline bool ContactAttemptAwaited(const SContactAttemptFacts &facts)
{
	if (facts.attempt == EContactRouteReport::None) {
		return false;
	}
	switch (facts.downloadState) {
	case DS_CONNECTING:
	case DS_WAITCALLBACK:
	case DS_WAITCALLBACKKAD:
		return true;
	case DS_LOWTOLOWIP:
		if (IsServerNatStep(facts.attempt) && facts.stepAgeMs < kNatStepAnswerWindowMs &&
			!(facts.attempt == EContactRouteReport::ServerHolePunch && facts.punchRefused)) {
			return true;
		}
		if (facts.attempt == EContactRouteReport::KadBuddy &&
			facts.stepAgeMs < kKadRendezvousAttemptMs) {
			return true;
		}
		break;
	default:
		break;
	}
	return facts.uploadState == US_CONNECTING;
}

struct SConnectionRouteFacts
{
	EConnectionOpener opener = EConnectionOpener::Accepted;
	//! A contact of ours was in flight when the hello came: a download or upload connect, a
	//! callback wait or a NAT-T step.
	bool contactInFlight = false;
	//! The route that contact went out on.
	EContactRouteReport attempt = EContactRouteReport::None;
};

//! The NAT-T step of ours that a connection on a punched path answers, if one is out.
inline std::optional<EConnectionRoute> OwnNatStep(const SConnectionRouteFacts &facts)
{
	if (!facts.contactInFlight) {
		return std::nullopt;
	}
	switch (facts.attempt) {
	case EContactRouteReport::ForeignNatCallback:
		return EConnectionRoute::ForeignNatCallback;
	case EContactRouteReport::ServerHolePunch:
		return EConnectionRoute::ServerHolePunch;
	case EContactRouteReport::LugdunumCallback:
		return EConnectionRoute::LugdunumCallback;
	default:
		return std::nullopt;
	}
}

inline EConnectionRoute DecideConnectionRoute(const SConnectionRouteFacts &facts)
{
	// A punched path serves whichever side asked: our own step names it, else the path's kind.
	switch (facts.opener) {
	case EConnectionOpener::Dial:
		return EConnectionRoute::Dial;
	case EConnectionOpener::AnswerServerCallback:
		return EConnectionRoute::AnsweredServerCallback;
	case EConnectionOpener::AnswerKadCallback:
		return EConnectionRoute::AnsweredKadCallback;
	case EConnectionOpener::AnswerDirectCallback:
		return EConnectionRoute::AnsweredDirectCallback;
	case EConnectionOpener::AnswerLugdunumCallback:
		return EConnectionRoute::AnsweredLugdunumCallback;
	case EConnectionOpener::HolePunchDial:
	case EConnectionOpener::AcceptedRendezvous:
		return OwnNatStep(facts).value_or(EConnectionRoute::ServerHolePunch);
	case EConnectionOpener::LugdunumCallbackDial:
		return OwnNatStep(facts).value_or(EConnectionRoute::LugdunumCallback);
	case EConnectionOpener::AcceptedLugdunumRendezvous:
		return OwnNatStep(facts).value_or(EConnectionRoute::AnsweredLugdunumCallback);
	case EConnectionOpener::KadRendezvousDial:
		// Only our own rendezvous request leads to this dial.
		return EConnectionRoute::KadBuddyRendezvous;
	case EConnectionOpener::AcceptedKadRendezvous:
		return OwnNatStep(facts).value_or(EConnectionRoute::AnsweredKadBuddyRendezvous);
	case EConnectionOpener::Accepted:
		break;
	}
	// Accepted: the answer to a callback we asked for, or a peer that came on its own.
	if (!facts.contactInFlight) {
		return EConnectionRoute::Inbound;
	}
	switch (facts.attempt) {
	case EContactRouteReport::ServerCallback:
		return EConnectionRoute::ServerCallback;
	case EContactRouteReport::KadBuddy:
		return EConnectionRoute::KadBuddyCallback;
	case EContactRouteReport::DirectUdpCallback:
		return EConnectionRoute::DirectUdpCallback;
	case EContactRouteReport::ForeignNatCallback:
		return EConnectionRoute::ForeignNatCallback;
	case EContactRouteReport::ServerHolePunch:
		return EConnectionRoute::ServerHolePunch;
	case EContactRouteReport::LugdunumCallback:
		return EConnectionRoute::LugdunumCallback;
	case EContactRouteReport::Dial:
	case EContactRouteReport::None:
		break;
	}
	return EConnectionRoute::Inbound;
}

//! EC_TAG_CLIENT_CONNECTION_ROUTE: the route in the low nibble, the transport in bits 4-5.
constexpr std::uint8_t ConnectionRouteMask = 0x0F;
constexpr std::uint8_t ConnectionTransportShift = 4;
constexpr std::uint8_t ConnectionTransportMask = 0x30;
constexpr std::uint8_t ConnectionIPv6Bit = 0x40;
constexpr std::uint8_t ConnectionProxyBit = 0x80;

inline std::uint8_t PackConnectionRoute(
	EConnectionRoute route, EConnectionTransport transport, bool overIPv6, bool viaProxy)
{
	static_assert(
		static_cast<unsigned>(EConnectionRoute::AnsweredKadBuddyRendezvous) <= ConnectionRouteMask,
		"every route must fit in the low nibble");
	return static_cast<std::uint8_t>(static_cast<std::uint8_t>(route) |
					 (static_cast<std::uint8_t>(transport) << ConnectionTransportShift) |
					 (overIPv6 ? ConnectionIPv6Bit : 0) |
					 (viaProxy ? ConnectionProxyBit : 0));
}

//! None for a route a newer daemon knows and this build does not.
inline EConnectionRoute UnpackConnectionRoute(std::uint8_t packed)
{
	const std::uint8_t route = packed & ConnectionRouteMask;
	return route <= static_cast<std::uint8_t>(EConnectionRoute::AnsweredKadBuddyRendezvous)
		       ? static_cast<EConnectionRoute>(route)
		       : EConnectionRoute::None;
}

//! TCP for a transport a newer daemon knows and this build does not.
inline EConnectionTransport UnpackConnectionTransport(std::uint8_t packed)
{
	const auto transport =
		static_cast<std::uint8_t>((packed & ConnectionTransportMask) >> ConnectionTransportShift);
	return transport <= static_cast<std::uint8_t>(EConnectionTransport::QUIC)
		       ? static_cast<EConnectionTransport>(transport)
		       : EConnectionTransport::TCP;
}

inline bool ConnectionOverIPv6(std::uint8_t packed)
{
	return (packed & ConnectionIPv6Bit) != 0;
}

inline bool ConnectionViaProxy(std::uint8_t packed)
{
	return (packed & ConnectionProxyBit) != 0;
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

//! The peer drops, unanswered, a direct callback request that comes within @a throttleMs of the
//! last one from our IP.
inline bool IsDirectCallbackThrottled(std::uint64_t lastSentMs, std::uint64_t nowMs, std::uint64_t throttleMs)
{
	return lastSentMs != 0 && nowMs >= lastSentMs && nowMs - lastSentMs < throttleMs;
}

struct SSourceRouteChoice
{
	ESourceRoute route = ESourceRoute::None;
	//! The direct callback was next, but the peer would drop it now.
	bool directHeld = false;
};

/**
 * NextSourceRoute() for a contact made now. A direct callback the peer would drop is skipped, not
 * failed: the next route is tried, and with none left the source waits for its next ask.
 */
inline SSourceRouteChoice ChooseSourceRoute(SSourceRouteFacts facts, bool directThrottled)
{
	SSourceRouteChoice choice;
	choice.directHeld = directThrottled && NextSourceRoute(facts) == ESourceRoute::DirectUdpCallback;
	facts.directCallback = facts.directCallback && !choice.directHeld;
	choice.route = NextSourceRoute(facts);
	return choice;
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

//! Adds @a mask to the channels in @a into. True when a set that had a channel gains another: the
//! first channel comes with the source and is not a second sighting.
inline bool MergeSourceOrigins(unsigned &into, unsigned mask)
{
	const unsigned added = mask & ~into;
	const bool counted = added != 0 && into != 0;
	into |= added;
	return counted;
}

//! Where a source was found, for the file it is a source for.
struct SSourceOrigin
{
	ESourceFrom from = SF_NONE;
	unsigned mask = 0;
};

//! The origin a client takes when it swaps to an A4AF file: that file's own, when it has one.
inline SSourceOrigin OriginAfterSwap(const SSourceOrigin &current, const SSourceOrigin &target)
{
	return target.from != SF_NONE ? target : current;
}

#endif // SOURCE_ROUTE_POLICY_H
