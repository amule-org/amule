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

TEST(SourceRoutePolicy, KnownServerIsKept)
{
	SSourceRouteMergeFacts facts = KadClientMeetsServerCopy();
	facts.existingHasServer = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeServerRoute);
	facts = KadClientMeetsServerCopy();
	facts.existingKadPlaceholderID = false;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeServerRoute);
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
	// A LowID with an IPv4 address, or a HighID, has a dial of its own.
	facts.existingHasIPv4 = true;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeLastResortIPv4);
	facts.existingHasIPv4 = false;
	facts.existingLowID = false;
	ASSERT_FALSE(DecideSourceRouteMerge(facts).takeLastResortIPv4);
}

TEST(SourceRoutePolicy, FamilySwitchPrefersIPv4AndRetriesOnlyTheAddressRoutes)
{
	unsigned failed = 0;
	for (ESourceRoute route : { ESourceRoute::Dial,
		     ESourceRoute::DirectUdpCallback,
		     ESourceRoute::ServerCallback,
		     ESourceRoute::KadBuddy }) {
		failed |= SourceRouteBit(route);
	}
	const unsigned callbacks =
		SourceRouteBit(ESourceRoute::ServerCallback) | SourceRouteBit(ESourceRoute::KadBuddy);

	SRouteFamilySwitch result = DecideRouteFamilySwitch(true, true, false, failed);
	ASSERT_TRUE(result.toIPv4);
	ASSERT_FALSE(result.toIPv6);
	ASSERT_EQUALS(callbacks, result.failed);

	result = DecideRouteFamilySwitch(false, true, false, failed);
	ASSERT_TRUE(result.toIPv6);
	ASSERT_EQUALS(callbacks, result.failed);

	result = DecideRouteFamilySwitch(true, false, true, failed);
	ASSERT_TRUE(result.toIPv4);
}

TEST(SourceRoutePolicy, NoFamilySwitchWithoutALastResort)
{
	const unsigned failed = SourceRouteBit(ESourceRoute::Dial);
	SRouteFamilySwitch result = DecideRouteFamilySwitch(false, false, false, failed);
	ASSERT_FALSE(result.toIPv4 || result.toIPv6);
	ASSERT_EQUALS(failed, result.failed);
	// An IPv6-only client is already on its IPv6.
	result = DecideRouteFamilySwitch(false, true, true, failed);
	ASSERT_FALSE(result.toIPv4 || result.toIPv6);
}
