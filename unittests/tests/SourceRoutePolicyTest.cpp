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

#include <muleunit/test.h>

#include "SourceRoutePolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(SourceRoutePolicy)

namespace
{
// A LowID with every callback route: direct callback, our server, a buddy, a foreign server.
SSourceRouteFacts LowIDWithEveryRoute()
{
	SSourceRouteFacts facts;
	facts.callbackOnly = true;
	facts.directCallback = true;
	facts.onOurServer = true;
	facts.hasBuddyID = true;
	facts.kadConnected = true;
	facts.foreignNatCallback = true;
	return facts;
}

SSourceRouteFacts HighID()
{
	SSourceRouteFacts facts;
	facts.canTakeCallback = true;
	facts.kadConnected = true;
	return facts;
}

void Fail(SSourceRouteFacts &facts, ESourceRoute route)
{
	facts.failed |= SourceRouteBit(route);
}
} // namespace

TEST(SourceRoutePolicy, LowIDTriesEveryRouteInOrder)
{
	SSourceRouteFacts facts = LowIDWithEveryRoute();
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::DirectUdpCallback);
	Fail(facts, ESourceRoute::DirectUdpCallback);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::ServerCallback);
	Fail(facts, ESourceRoute::ServerCallback);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::KadBuddy);
	Fail(facts, ESourceRoute::KadBuddy);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::ForeignNatCallback);
	Fail(facts, ESourceRoute::ForeignNatCallback);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::None);
}

TEST(SourceRoutePolicy, LowIDOffersOnlyTheRoutesItHas)
{
	SSourceRouteFacts facts;
	facts.callbackOnly = true;
	facts.onOurServer = true;
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::ServerCallback);
	Fail(facts, ESourceRoute::ServerCallback);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::None);
}

TEST(SourceRoutePolicy, BuddyNeedsKad)
{
	SSourceRouteFacts facts = LowIDWithEveryRoute();
	facts.directCallback = false;
	facts.onOurServer = false;
	facts.kadConnected = false;
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::ForeignNatCallback);
	facts.foreignNatCallback = false;
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::None);
}

TEST(SourceRoutePolicy, HighIDIsDialled)
{
	SSourceRouteFacts facts = HighID();
	facts.hasBuddyID = true;
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::Dial);
}

TEST(SourceRoutePolicy, FailedHighIDDialMovesToItsBuddy)
{
	SSourceRouteFacts facts = HighID();
	facts.hasBuddyID = true;
	Fail(facts, ESourceRoute::Dial);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::KadBuddy);
	Fail(facts, ESourceRoute::KadBuddy);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::None);
}

TEST(SourceRoutePolicy, HighIDBuddyNeedsUsReachable)
{
	SSourceRouteFacts facts = HighID();
	facts.hasBuddyID = true;
	facts.canTakeCallback = false;
	Fail(facts, ESourceRoute::Dial);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::None);
}

TEST(SourceRoutePolicy, FailedHighIDDialWithoutBuddyGivesUp)
{
	SSourceRouteFacts facts = HighID();
	Fail(facts, ESourceRoute::Dial);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::None);
}

TEST(SourceRoutePolicy, DirectCallbackComesBeforeTheDial)
{
	// A LowID dialled over IPv6 that also takes a direct callback: the callback goes first, as before.
	SSourceRouteFacts facts = HighID();
	facts.directCallback = true;
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::DirectUdpCallback);
	Fail(facts, ESourceRoute::DirectUdpCallback);
	ASSERT_TRUE(NextSourceRoute(facts) == ESourceRoute::Dial);
}

namespace
{
// A Kad buddy client (ID 1, no server, no address) meeting a server copy of the same source.
SSourceRouteMergeFacts KadClientMeetsServerCopy()
{
	SSourceRouteMergeFacts facts;
	facts.existingKadPlaceholderID = true;
	facts.existingLowID = true;
	facts.incomingServerLowID = true;
	return facts;
}
} // namespace

TEST(SourceRoutePolicy, KadClientTakesTheServerRoute)
{
	ASSERT_TRUE(DecideSourceRouteMerge(KadClientMeetsServerCopy()).takeServerRoute);
}

TEST(SourceRoutePolicy, OtherServerRouteIsAFallback)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.existingHasServer = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.takeServerRoute);
	ASSERT_TRUE(merge.storeServerCandidate);
	facts.existingHasServerCandidate = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).storeServerCandidate);
	facts.sameServerRoute = true;
	facts.existingHasServerCandidate = false;
	merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.takeServerRoute || merge.storeServerCandidate);
}

TEST(SourceRoutePolicy, OurServerReplacesTheServerRoute)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.existingHasServer = true;
	facts.incomingTrusted = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.takeServerRoute);
	ASSERT_FALSE(merge.storeServerCandidate);
	// The replaced route stays as a fallback: the server does not check the hash a client claims.
	ASSERT_TRUE(merge.keepOldServerRouteAsCandidate);
	ASSERT_FALSE(merge.keepOldHighIDAsLastResort);
}

TEST(SourceRoutePolicy, OurServerTurnsAHighIDIntoALowID)
{
	SSourceRouteMergeFacts facts;
	facts.existingHasIPv4 = true;
	facts.incomingServerLowID = true;
	facts.incomingTrusted = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.takeServerRoute);
	ASSERT_TRUE(merge.keepOldHighIDAsLastResort);
	ASSERT_FALSE(merge.keepOldServerRouteAsCandidate);
	// A HighID hears a LowID claim from anywhere else as a fallback only.
	facts.incomingTrusted = false;
	merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.takeServerRoute);
	ASSERT_TRUE(merge.storeServerCandidate);
}

TEST(SourceRoutePolicy, OurServerReplacesTheHighIDAddress)
{
	SSourceRouteMergeFacts facts;
	facts.existingHasIPv4 = true;
	facts.incomingHighIDIPv4 = true;
	facts.incomingTrusted = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.replaceWithHighID);
	ASSERT_TRUE(merge.keepOldHighIDAsLastResort);
	ASSERT_FALSE(merge.takeLastResortIPv4);
	facts.incomingTrusted = false;
	merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.replaceWithHighID);
	ASSERT_TRUE(merge.takeLastResortIPv4);
	facts.sameHighID = true;
	merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.replaceWithHighID || merge.takeLastResortIPv4);
}

TEST(SourceRoutePolicy, OurServerTurnsALowIDIntoAHighID)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.incomingServerLowID = false;
	facts.incomingHighIDIPv4 = true;
	facts.incomingTrusted = true;
	ASSERT_TRUE(DecideSourceRouteMerge(facts).replaceWithHighID);
	// A LowID with a server route keeps it as a fallback.
	facts.existingHasServer = true;
	const SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.replaceWithHighID);
	ASSERT_TRUE(merge.keepOldServerRouteAsCandidate);
	ASSERT_FALSE(merge.keepOldHighIDAsLastResort);
}

TEST(SourceRoutePolicy, IPv6OnlyClientKeepsItsIdentityEvenForOurServer)
{
	SSourceRouteMergeFacts facts;
	facts.existingNativeIPv6 = true;
	facts.existingLowID = true;
	facts.incomingHighIDIPv4 = true;
	facts.incomingTrusted = true;
	const SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.replaceWithHighID);
	ASSERT_TRUE(merge.takeLastResortIPv4);
}

TEST(SourceRoutePolicy, LowIDTakesADirectCallback)
{
	SSourceRouteMergeFacts facts;
	facts.existingLowID = true;
	facts.incomingDirectCallback = true;
	ASSERT_TRUE(DecideSourceRouteMerge(facts).takeDirectCallback);
	facts.existingHasIPv4 = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeDirectCallback);
	facts.sameIPv4 = true;
	ASSERT_TRUE(DecideSourceRouteMerge(facts).takeDirectCallback);
	facts.existingDirectCallback = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeDirectCallback);
}

TEST(SourceRoutePolicy, HighIDTakesNoDirectCallback)
{
	SSourceRouteMergeFacts facts;
	facts.incomingDirectCallback = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeDirectCallback);
}

TEST(SourceRoutePolicy, ClientWithIPv4TakesAnAdvertisedIPv6)
{
	SSourceRouteMergeFacts facts;
	facts.existingHasIPv4 = true;
	facts.incomingRoutableIPv6 = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.takeAdvertisedIPv6);
	ASSERT_FALSE(merge.takeLastResortIPv6);
	facts.existingHasAdvertisedIPv6 = true;
	merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.takeAdvertisedIPv6);
	ASSERT_FALSE(merge.takeLastResortIPv6);
}

TEST(SourceRoutePolicy, IPv4TakenWithADirectCallbackMakesTheIPv6Advertised)
{
	SSourceRouteMergeFacts facts;
	facts.existingLowID = true;
	facts.incomingDirectCallback = true;
	facts.incomingRoutableIPv6 = true;
	ASSERT_TRUE(DecideSourceRouteMerge(facts).takeAdvertisedIPv6);
}

TEST(SourceRoutePolicy, ClientWithoutIPv4KeepsTheIPv6AsALastResort)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.incomingServerLowID = false;
	facts.incomingRoutableIPv6 = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_FALSE(merge.takeAdvertisedIPv6);
	ASSERT_TRUE(merge.takeLastResortIPv6);
	facts.existingHasLastResortIPv6 = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeLastResortIPv6);
}

TEST(SourceRoutePolicy, IPv6OnlyClientKeepsAnIPv4CopyAsALastResort)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.incomingDirectCallback = true;
	facts.incomingRoutableIPv6 = true;
	facts.existingNativeIPv6 = true;
	SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.takeServerRoute);
	ASSERT_FALSE(merge.takeDirectCallback);
	ASSERT_FALSE(merge.takeAdvertisedIPv6);
	ASSERT_FALSE(merge.takeLastResortIPv6);
	ASSERT_TRUE(merge.takeLastResortIPv4);

	facts.incomingDirectCallback = false;
	facts.incomingHighIDIPv4 = true;
	ASSERT_TRUE(DecideSourceRouteMerge(facts).takeLastResortIPv4);
	facts.existingHasLastResortIPv4 = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeLastResortIPv4);
}

TEST(SourceRoutePolicy, IPv6OnlyClientTakesNoIPv4FromALowIDServerCopy)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.existingNativeIPv6 = true;
	const SSourceRouteMerge merge = DecideSourceRouteMerge(facts);
	ASSERT_TRUE(merge.takeServerRoute);
	ASSERT_FALSE(merge.takeLastResortIPv4);
}

TEST(SourceRoutePolicy, AddresslessLowIDKeepsAHighIDCopyAsALastResort)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.incomingServerLowID = false;
	facts.incomingHighIDIPv4 = true;
	ASSERT_TRUE(DecideSourceRouteMerge(facts).takeLastResortIPv4);
	facts.existingHasLastResortIPv4 = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeLastResortIPv4);
}

namespace
{
std::vector<ESourceFallback> TriedFallbacks(
	const SSourceFallbacks &available, ESourceFallback usable, bool &used)
{
	std::vector<ESourceFallback> tried;
	used = UseFirstUsableFallback(available, [&tried, usable](ESourceFallback fallback) {
		tried.push_back(fallback);
		return fallback == usable;
	});
	return tried;
}
} // namespace

TEST(SourceRoutePolicy, FallbacksGoServerRouteThenIPv4ThenIPv6)
{
	SSourceFallbacks available;
	available.serverCandidate = true;
	available.lastResortIPv4 = true;
	available.lastResortIPv6 = true;
	bool used = false;
	const std::vector<ESourceFallback> tried =
		TriedFallbacks(available, ESourceFallback::ServerCandidate, used);
	ASSERT_TRUE(used);
	ASSERT_EQUALS(1u, tried.size());
}

TEST(SourceRoutePolicy, AFallbackWithoutARouteMovesOnToTheNext)
{
	SSourceFallbacks available;
	available.serverCandidate = true;
	available.lastResortIPv4 = true;
	available.lastResortIPv6 = true;
	bool used = false;
	std::vector<ESourceFallback> tried = TriedFallbacks(available, ESourceFallback::LastResortIPv6, used);
	ASSERT_TRUE(used);
	ASSERT_EQUALS(3u, tried.size());
	ASSERT_TRUE(tried[0] == ESourceFallback::ServerCandidate);
	ASSERT_TRUE(tried[1] == ESourceFallback::LastResortIPv4);
	ASSERT_TRUE(tried[2] == ESourceFallback::LastResortIPv6);

	// None usable: every one is spent and the source is given up.
	available.lastResortIPv6 = false;
	tried = TriedFallbacks(available, ESourceFallback::LastResortIPv6, used);
	ASSERT_FALSE(used);
	ASSERT_EQUALS(2u, tried.size());
}

TEST(SourceRoutePolicy, AnIPv6OnlyClientHasNoIPv6Fallback)
{
	SSourceFallbacks available;
	available.lastResortIPv6 = true;
	available.nativeIPv6 = true;
	bool used = true;
	ASSERT_EQUALS(0u, TriedFallbacks(available, ESourceFallback::LastResortIPv6, used).size());
	ASSERT_FALSE(used);
}

TEST(SourceRoutePolicy, AFallbackRetriesOnlyTheRoutesItChanges)
{
	unsigned failed = 0;
	for (ESourceRoute route : { ESourceRoute::Dial,
		     ESourceRoute::DirectUdpCallback,
		     ESourceRoute::ServerCallback,
		     ESourceRoute::KadBuddy }) {
		failed |= SourceRouteBit(route);
	}
	const unsigned addressRoutes =
		SourceRouteBit(ESourceRoute::Dial) | SourceRouteBit(ESourceRoute::DirectUdpCallback);
	ASSERT_EQUALS(failed & ~SourceRouteBit(ESourceRoute::ServerCallback),
		FailedAfterFallback(ESourceFallback::ServerCandidate, failed));
	ASSERT_EQUALS(failed & ~addressRoutes, FailedAfterFallback(ESourceFallback::LastResortIPv4, failed));
	ASSERT_EQUALS(failed & ~addressRoutes, FailedAfterFallback(ESourceFallback::LastResortIPv6, failed));
}

// EC_TAG_CLIENT_CONTACT_ROUTE packs the route and its two flags into one byte.
TEST(SourceRoutePolicy, AContactRouteSurvivesItsPacking)
{
	for (EContactRouteReport route : { EContactRouteReport::None,
		     EContactRouteReport::Dial,
		     EContactRouteReport::DirectUdpCallback,
		     EContactRouteReport::ServerCallback,
		     EContactRouteReport::KadBuddy,
		     EContactRouteReport::ForeignNatCallback,
		     EContactRouteReport::ServerHolePunch,
		     EContactRouteReport::LugdunumCallback }) {
		for (int flags = 0; flags < 8; ++flags) {
			const bool unproven = (flags & 1) != 0;
			const bool fallback = (flags & 2) != 0;
			const bool overIPv6 = (flags & 4) != 0;
			const std::uint8_t packed = PackContactRoute(route, unproven, fallback, overIPv6);
			ASSERT_TRUE(UnpackContactRoute(packed) == route);
			ASSERT_EQUALS(unproven, ContactRouteIsUnproven(packed));
			ASSERT_EQUALS(fallback, ContactRouteFromFallback(packed));
			ASSERT_EQUALS(overIPv6, ContactRouteOverIPv6(packed));
		}
	}
	// The chain's own routes report under the same numbers.
	ASSERT_TRUE(ReportedRoute(ESourceRoute::KadBuddy) == EContactRouteReport::KadBuddy);
	ASSERT_TRUE(ReportedRoute(ESourceRoute::None) == EContactRouteReport::None);
}

// A route a newer daemon knows and this build does not reads as none, flags and all.
TEST(SourceRoutePolicy, AnUnknownPackedRouteReadsAsNone)
{
	const std::uint8_t packed = 0x0F | ContactRouteUnprovenBit | ContactRouteFallbackBit;
	ASSERT_TRUE(UnpackContactRoute(packed) == EContactRouteReport::None);
	ASSERT_TRUE(ContactRouteIsUnproven(packed));
	ASSERT_TRUE(ContactRouteFromFallback(packed));
}

// EC_TAG_CLIENT_CONNECTION_ROUTE packs the route, the transport and two flags into one byte.
TEST(SourceRoutePolicy, AConnectionRouteSurvivesItsPacking)
{
	for (unsigned r = 0; r <= static_cast<unsigned>(EConnectionRoute::AnsweredLugdunumCallback); ++r) {
		const auto route = static_cast<EConnectionRoute>(r);
		for (EConnectionTransport transport : { EConnectionTransport::TCP,
			     EConnectionTransport::UTP,
			     EConnectionTransport::QUIC }) {
			for (int flags = 0; flags < 4; ++flags) {
				const bool overIPv6 = (flags & 1) != 0;
				const bool viaProxy = (flags & 2) != 0;
				const std::uint8_t packed =
					PackConnectionRoute(route, transport, overIPv6, viaProxy);
				ASSERT_TRUE(UnpackConnectionRoute(packed) == route);
				ASSERT_TRUE(UnpackConnectionTransport(packed) == transport);
				ASSERT_EQUALS(overIPv6, ConnectionOverIPv6(packed));
				ASSERT_EQUALS(viaProxy, ConnectionViaProxy(packed));
			}
		}
	}
	// A route or transport a newer daemon knows reads as none and TCP.
	ASSERT_TRUE(UnpackConnectionRoute(0x0F) == EConnectionRoute::None);
	ASSERT_TRUE(UnpackConnectionTransport(0x30) == EConnectionTransport::TCP);
}

// A socket we opened names its own reason; a socket we accepted is the answer to the contact in
// flight, or a peer that came on its own.
TEST(SourceRoutePolicy, AConnectionRouteFollowsWhoOpenedIt)
{
	SConnectionRouteFacts facts;
	facts.contactInFlight = true;
	facts.attempt = EContactRouteReport::ServerCallback;

	const std::pair<EConnectionOpener, EConnectionRoute> opened[] = {
		{ EConnectionOpener::Dial, EConnectionRoute::Dial },
		{ EConnectionOpener::AnswerServerCallback, EConnectionRoute::AnsweredServerCallback },
		{ EConnectionOpener::AnswerKadCallback, EConnectionRoute::AnsweredKadCallback },
		{ EConnectionOpener::AnswerDirectCallback, EConnectionRoute::AnsweredDirectCallback },
		{ EConnectionOpener::HolePunchDial, EConnectionRoute::ServerHolePunch },
		{ EConnectionOpener::LugdunumCallbackDial, EConnectionRoute::LugdunumCallback },
		{ EConnectionOpener::AnswerLugdunumCallback, EConnectionRoute::AnsweredLugdunumCallback },
	};
	for (const auto &entry : opened) {
		facts.opener = entry.first;
		ASSERT_TRUE(DecideConnectionRoute(facts) == entry.second);
	}

	facts.opener = EConnectionOpener::Accepted;
	const std::pair<EContactRouteReport, EConnectionRoute> answered[] = {
		{ EContactRouteReport::ServerCallback, EConnectionRoute::ServerCallback },
		{ EContactRouteReport::KadBuddy, EConnectionRoute::KadBuddyCallback },
		{ EContactRouteReport::DirectUdpCallback, EConnectionRoute::DirectUdpCallback },
		{ EContactRouteReport::ForeignNatCallback, EConnectionRoute::ForeignNatCallback },
		{ EContactRouteReport::ServerHolePunch, EConnectionRoute::ServerHolePunch },
		{ EContactRouteReport::LugdunumCallback, EConnectionRoute::LugdunumCallback },
		// Our dial was in flight, but the peer reached us first.
		{ EContactRouteReport::Dial, EConnectionRoute::Inbound },
		{ EContactRouteReport::None, EConnectionRoute::Inbound },
	};
	for (const auto &entry : answered) {
		facts.attempt = entry.first;
		ASSERT_TRUE(DecideConnectionRoute(facts) == entry.second);
	}

	// A stream from a peer a rendezvous announced is that NAT-T meeting, named by our own step.
	facts.opener = EConnectionOpener::AcceptedRendezvous;
	facts.attempt = EContactRouteReport::LugdunumCallback;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::LugdunumCallback);
	facts.attempt = EContactRouteReport::ForeignNatCallback;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::ForeignNatCallback);
	facts.attempt = EContactRouteReport::ServerHolePunch;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::ServerHolePunch);
	// Without a step of ours, we answered the peer's hole punch or Lugdunum callback.
	facts.contactInFlight = false;
	facts.attempt = EContactRouteReport::None;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::ServerHolePunch);
	facts.opener = EConnectionOpener::AcceptedLugdunumRendezvous;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::AnsweredLugdunumCallback);
	facts.contactInFlight = true;
	facts.attempt = EContactRouteReport::LugdunumCallback;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::LugdunumCallback);
	// Our dial on a path our cross-server request opened is that request.
	facts.opener = EConnectionOpener::HolePunchDial;
	facts.attempt = EContactRouteReport::ForeignNatCallback;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::ForeignNatCallback);
	facts.contactInFlight = false;
	facts.opener = EConnectionOpener::Accepted;
	facts.contactInFlight = true;

	// A callback we asked for long ago does not explain a peer that dials us now.
	facts.contactInFlight = false;
	facts.attempt = EContactRouteReport::ServerCallback;
	ASSERT_TRUE(DecideConnectionRoute(facts) == EConnectionRoute::Inbound);
}

// The callback request a socket answers names it; only without one do the NAT-T dial flags.
TEST(SourceRoutePolicy, AnAnsweredCallbackNamesTheSocket)
{
	ASSERT_TRUE(
		ResolveConnectionOpener(EConnectionOpener::Dial, false, false) == EConnectionOpener::Dial);
	ASSERT_TRUE(ResolveConnectionOpener(EConnectionOpener::Dial, false, true) ==
		    EConnectionOpener::HolePunchDial);
	// A Lugdunum dial is a hole-punch dial too; the Lugdunum flag is the more specific one.
	ASSERT_TRUE(ResolveConnectionOpener(EConnectionOpener::Dial, true, true) ==
		    EConnectionOpener::LugdunumCallbackDial);
	ASSERT_TRUE(ResolveConnectionOpener(EConnectionOpener::AnswerLugdunumCallback, true, true) ==
		    EConnectionOpener::AnswerLugdunumCallback);
	ASSERT_TRUE(ResolveConnectionOpener(EConnectionOpener::AnswerServerCallback, false, false) ==
		    EConnectionOpener::AnswerServerCallback);
}

// A LowID-to-LowID wait is ours only while a NAT-T step is fresh; the other contact states always
// are, and an upload callback of ours names the answer with its own route.
TEST(SourceRoutePolicy, AContactIsInFlightOnlyWhileOursIsAwaited)
{
	const auto inFlight = [](std::uint8_t downloadState,
				      std::uint8_t uploadState,
				      EContactRouteReport attempt,
				      std::uint64_t stepAgeMs = 0) {
		SContactInFlightFacts facts;
		facts.downloadState = downloadState;
		facts.uploadState = uploadState;
		facts.attempt = attempt;
		facts.natStepAgeMs = stepAgeMs;
		return ContactInFlight(facts);
	};
	for (std::uint8_t state : { DS_CONNECTING, DS_WAITCALLBACK, DS_WAITCALLBACKKAD }) {
		ASSERT_TRUE(inFlight(state, US_NONE, EContactRouteReport::None).has_value());
	}
	for (EContactRouteReport step : { EContactRouteReport::ForeignNatCallback,
		     EContactRouteReport::ServerHolePunch,
		     EContactRouteReport::LugdunumCallback }) {
		ASSERT_TRUE(inFlight(DS_LOWTOLOWIP, US_NONE, step) == step);
		// The step was answered or lost long ago: the state outlives it while we stay LowID.
		ASSERT_FALSE(inFlight(DS_LOWTOLOWIP, US_NONE, step, kNatStepAnswerWindowMs).has_value());
	}
	// No step was sent this round, or the route is an older attempt's.
	ASSERT_FALSE(inFlight(DS_LOWTOLOWIP, US_NONE, EContactRouteReport::None).has_value());
	ASSERT_FALSE(inFlight(DS_LOWTOLOWIP, US_NONE, EContactRouteReport::ServerCallback).has_value());
	ASSERT_FALSE(inFlight(DS_ONQUEUE, US_NONE, EContactRouteReport::ServerCallback).has_value());
	ASSERT_FALSE(inFlight(DS_NONE, US_NONE, EContactRouteReport::Dial).has_value());
	// An upload connect is a contact of ours as well.
	ASSERT_TRUE(inFlight(DS_NONE, US_CONNECTING, EContactRouteReport::None).has_value());

	// A queued uploader we asked to call us back: its answer is that callback, not a peer that
	// came on its own.
	SContactInFlightFacts facts;
	facts.uploadState = US_ONUPLOADQUEUE;
	facts.uploadCallbackInFlight = true;
	facts.uploadCallbackRoute = EContactRouteReport::ServerHolePunch;
	ASSERT_TRUE(ContactInFlight(facts) == EContactRouteReport::ServerHolePunch);
	// Our download contact, when one is out, comes first.
	facts.downloadState = DS_WAITCALLBACK;
	facts.attempt = EContactRouteReport::KadBuddy;
	ASSERT_TRUE(ContactInFlight(facts) == EContactRouteReport::KadBuddy);
}

// Channels merge into a set; only a set that already had one counts the gain.
TEST(SourceRoutePolicy, SourceOriginsMergeAndCountOnlyLaterChannels)
{
	unsigned mask = 0;
	ASSERT_FALSE(MergeSourceOrigins(mask, SourceFromBit(SF_KADEMLIA)));
	ASSERT_EQUALS(SourceFromBit(SF_KADEMLIA), mask);
	ASSERT_FALSE(MergeSourceOrigins(mask, SourceFromBit(SF_KADEMLIA)));
	ASSERT_TRUE(MergeSourceOrigins(mask, SourceFromBit(SF_LOCAL_SERVER)));
	ASSERT_EQUALS(SourceFromBit(SF_KADEMLIA) | SourceFromBit(SF_LOCAL_SERVER), mask);
	ASSERT_FALSE(MergeSourceOrigins(mask, 0));
}

// A swap takes the target file's own origin and leaves the current one only for a target without.
TEST(SourceRoutePolicy, ASwapTakesTheTargetFilesOrigin)
{
	const SSourceOrigin current{ SF_KADEMLIA, SourceFromBit(SF_KADEMLIA) };
	const SSourceOrigin target{ SF_PASSIVE,
		SourceFromBit(SF_PASSIVE) | SourceFromBit(SF_SOURCE_EXCHANGE) };
	const SSourceOrigin swapped = OriginAfterSwap(current, target);
	ASSERT_TRUE(swapped.from == SF_PASSIVE);
	ASSERT_EQUALS(target.mask, swapped.mask);

	const SSourceOrigin kept = OriginAfterSwap(current, SSourceOrigin{});
	ASSERT_TRUE(kept.from == SF_KADEMLIA);
	ASSERT_EQUALS(current.mask, kept.mask);
}
