//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include "DeadSourceKey.h"
#include "StoredSourceRoute.h"

#include <vector>
using namespace muleunit;
DECLARE_SIMPLE(CDeadSourceKey)

namespace
{
CNetworkAddress IPv6(unsigned char last)
{
	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 1;
	bytes[15] = last;
	return CNetworkAddress::IPv6FromOctets(bytes);
}

CDeadSourceKey Key(const CProtocolPeerIdentity &identity, uint32 id, uint16 tcp, uint32 server, uint16 kad)
{
	return CDeadSourceKey(identity, id, tcp, server, kad);
}
} // namespace

TEST(CDeadSourceKey, NativeIPv6UsesAddressBucketAndMatchesPorts)
{
	const auto first = Key(CProtocolPeerIdentity::FromNativeIPv6(IPv6(1)), 0, 4662, 0, 4672);
	const auto second = Key(CProtocolPeerIdentity::FromNativeIPv6(IPv6(2)), 0, 4662, 0, 4672);
	ASSERT_FALSE(first.Matches(second));
	ASSERT_TRUE(first < second || second < first);
	ASSERT_FALSE(first < second && second < first);

	const auto sameTcp = Key(CProtocolPeerIdentity::FromNativeIPv6(IPv6(1)), 42, 4662, 99, 0);
	const auto sameKad = Key(CProtocolPeerIdentity::FromNativeIPv6(IPv6(1)), 0, 1, 0, 4672);
	ASSERT_TRUE(first.Matches(sameTcp));
	ASSERT_TRUE(first.Matches(sameKad));
}

TEST(CDeadSourceKey, NativeIPv6DoesNotMatchLegacy)
{
	const auto native = Key(CProtocolPeerIdentity::FromNativeIPv6(IPv6(1)), 0, 4662, 0, 4672);
	const auto legacy = Key(CProtocolPeerIdentity::FromServerScopedLowID(
					42, CNetworkAddress::FromIPv4NetworkOrder(0x0100000a), 4662),
		0,
		4662,
		0x0a000001,
		4672);
	ASSERT_FALSE(native.Matches(legacy));
}

TEST(CDeadSourceKey, LegacyPreservesIDAndServerSemantics)
{
	const auto high = CProtocolPeerIdentity::FromIPv4HighID(0xC0000201);
	ASSERT_TRUE(Key(high, 0xC0000201, 4662, 1, 0).Matches(Key(high, 0xC0000201, 4662, 2, 0)));

	const auto low = CProtocolPeerIdentity::FromServerScopedLowID(
		42, CNetworkAddress::FromIPv4NetworkOrder(0x0100000a), 4662);
	ASSERT_FALSE(Key(low, 42, 4662, 0x0a000001, 0).Matches(Key(low, 42, 4662, 0x0a000002, 0)));
	ASSERT_TRUE(Key(low, 42, 4662, 0x0a000001, 0).Matches(Key(low, 42, 4662, 0x0a000001, 0)));
}

namespace
{
// CDeadSource::operator== before native IPv6 keys existed.
bool LegacyMatches(uint32 id,
	uint16 tcp,
	uint32 server,
	uint16 kad,
	uint32 otherID,
	uint16 otherTcp,
	uint32 otherServer,
	uint16 otherKad)
{
	if (id != otherID || (tcp != otherTcp && kad != otherKad))
		return false;
	return !IsLowID(id) || server == otherServer;
}
} // namespace

// Without native IPv6 (every build without ENABLE_IPV6), matching and bucketing must be
// exactly what they were when the list was keyed on the hybrid ID alone.
TEST(CDeadSourceKey, NonNativeKeysBehaveExactlyAsBefore)
{
	const uint32 ids[] = { 0, 42, 0xC0000201, 0xC0000202 };
	const uint16 tcpPorts[] = { 0, 4662, 4663 };
	const uint16 kadPorts[] = { 0, 4672, 4673 };
	const uint32 servers[] = { 0x0100000A, 0x0200000A };
	struct SCase
	{
		uint32 id;
		uint16 tcp;
		uint32 server;
		uint16 kad;
	};
	std::vector<SCase> cases;
	for (uint32 id : ids)
		for (uint16 tcp : tcpPorts)
			for (uint16 kad : kadPorts)
				for (uint32 server : servers)
					cases.push_back({ id, tcp, server, kad });
	const auto key = [](const SCase &c) {
		return Key(CProtocolPeerIdentity::FromClientState(
				   c.id, IsLowID(c.id), CNetworkAddress::Absent(), c.server, 4661),
			c.id,
			c.tcp,
			c.server,
			c.kad);
	};
	for (const SCase &a : cases) {
		for (const SCase &b : cases) {
			ASSERT_EQUALS(
				LegacyMatches(a.id, a.tcp, a.server, a.kad, b.id, b.tcp, b.server, b.kad),
				key(a).Matches(key(b)));
			ASSERT_EQUALS(a.id < b.id, key(a) < key(b));
		}
	}
}

namespace
{
// The key CDeadSourceList builds for a source record, before any hello: a HighID record carries no
// user address yet, a server LowID no address at all, an IPv6 record its address.
CDeadSourceKey RecordKey(
	uint32 id, const CNetworkAddress &user, uint16 tcp, uint32 server, uint16 serverPort, uint16 kad)
{
	return Key(CProtocolPeerIdentity::FromClientState(id, IsLowID(id), user, server, serverPort),
		id,
		tcp,
		server,
		kad);
}

const uint32 kHighID = 0x0A0B0C0Du;
const uint32 kServer = 0x0100007Fu;
} // namespace

TEST(CDeadSourceKey, AStoredHighIDMatchesItsLaterRecord)
{
	SStoredSourceRoute route;
	route.id = kHighID;
	route.address = CNetworkAddress::FromIPv4NetworkOrder(0x0D0C0B0Au);
	route.tcpPort = 4662;
	ASSERT_TRUE(route.DeadKey().Matches(RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 0)));
	ASSERT_FALSE(
		route.DeadKey().Matches(RecordKey(kHighID + 1, CNetworkAddress::Absent(), 4662, 0, 0, 0)));
}

TEST(CDeadSourceKey, AStoredServerRouteMatchesItsLaterRecord)
{
	SStoredSourceRoute route;
	route.id = 0x1235;
	route.serverIP = kServer;
	route.serverPort = 4661;
	route.tcpPort = 4662;
	ASSERT_TRUE(route.DeadKey().Matches(
		RecordKey(0x1235, CNetworkAddress::Absent(), 4662, kServer, 4661, 0)));
	// Another server's LowID 0x1235 is another client.
	ASSERT_FALSE(route.DeadKey().Matches(
		RecordKey(0x1235, CNetworkAddress::Absent(), 4662, kServer + 1, 4661, 0)));
}

TEST(CDeadSourceKey, AStoredIPv6MatchesItsLaterRecord)
{
	SStoredSourceRoute route;
	route.address = IPv6(7);
	route.tcpPort = 4662;
	ASSERT_TRUE(route.DeadKey().Matches(RecordKey(1, IPv6(7), 4662, 0, 0, 0)));
	ASSERT_FALSE(route.DeadKey().Matches(RecordKey(1, IPv6(8), 4662, 0, 0, 0)));
}

TEST(CDeadSourceKey, ARouteWhereAnotherHostAnsweredIsNotDead)
{
	SStoredSourceRoute current;
	current.id = kHighID;
	current.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	current.tcpPort = 4662;
	const CDeadSourceKey left = RecordKey(0x1235, CNetworkAddress::Absent(), 4662, kServer, 4661, 0);
	const CDeadSourceKey otherHost = current.DeadKey();

	const std::vector<CDeadSourceKey> keys = DeadSourceKeysFor(current, {}, { left }, { otherHost });
	ASSERT_EQUALS(1u, keys.size());
	ASSERT_TRUE(keys[0].Matches(left));

	// Also when the other host answered on a route the source left.
	SStoredSourceRoute now;
	now.id = 0x1236;
	now.serverIP = kServer;
	now.serverPort = 4661;
	now.tcpPort = 4662;
	const std::vector<CDeadSourceKey> keysLeft = DeadSourceKeysFor(now, {}, { left }, { left });
	ASSERT_EQUALS(1u, keysLeft.size());
	ASSERT_TRUE(
		keysLeft[0].Matches(RecordKey(0x1236, CNetworkAddress::Absent(), 4662, kServer, 4661, 0)));
}

TEST(CDeadSourceKey, AnOtherHostKeyIsStoredOnce)
{
	std::vector<CDeadSourceKey> keys;
	const CDeadSourceKey first = RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 4672);
	AddOtherHostKey(keys, first);
	ASSERT_EQUALS(1u, keys.size());
	AddOtherHostKey(keys, RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 4672));
	ASSERT_EQUALS(1u, keys.size());

	// Same ID and TCP port, another Kad port: it Matches the stored key but is not equal to it.
	const CDeadSourceKey otherKad = RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 4673);
	ASSERT_TRUE(otherKad.Matches(first));
	AddOtherHostKey(keys, otherKad);
	ASSERT_EQUALS(2u, keys.size());
}

TEST(CDeadSourceKey, OnlyTheAnsweredRouteLosesItsOtherHostKey)
{
	std::vector<CDeadSourceKey> keys;
	const CDeadSourceKey first = RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 4672);
	const CDeadSourceKey otherKad = RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 4673);
	AddOtherHostKey(keys, first);
	AddOtherHostKey(keys, otherKad);
	// The client's own hash answered the first route: only that key goes.
	RemoveOtherHostKey(keys, RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 4672));
	ASSERT_EQUALS(1u, keys.size());
	ASSERT_TRUE(keys[0] == otherKad);
	RemoveOtherHostKey(keys, first);
	ASSERT_EQUALS(1u, keys.size());
}

TEST(CDeadSourceKey, ADeadSourceIsKeyedUnderEveryRoute)
{
	SStoredSourceRoute current;
	current.id = 0x1235;
	current.serverIP = kServer;
	current.serverPort = 4661;
	current.tcpPort = 4662;
	SStoredSourceRoute lastResortIPv6;
	lastResortIPv6.address = IPv6(7);
	lastResortIPv6.tcpPort = 4662;
	const CDeadSourceKey former = RecordKey(kHighID, CNetworkAddress::Absent(), 4662, 0, 0, 0);

	const std::vector<CDeadSourceKey> keys =
		DeadSourceKeysFor(current, { SStoredSourceRoute(), lastResortIPv6 }, { former });
	ASSERT_EQUALS(3u, keys.size());
	ASSERT_TRUE(keys[0].Matches(RecordKey(0x1235, CNetworkAddress::Absent(), 4662, kServer, 4661, 0)));
	ASSERT_TRUE(keys[1].Matches(RecordKey(1, IPv6(7), 4662, 0, 0, 0)));
	ASSERT_TRUE(keys[2].Matches(former));
}

DECLARE_SIMPLE(CSourceRouteChainStart)

namespace
{
SStoredSourceRoute ServerRoute(uint32 id, uint32 server)
{
	SStoredSourceRoute route;
	route.id = id;
	route.serverIP = server;
	route.serverPort = 4661;
	route.tcpPort = 4662;
	return route;
}

// A LowID on our server, with a fallback server route and a last-resort HighID.
SSourceRoutes ChainStartRoutes()
{
	SSourceRoutes routes;
	routes.route = ServerRoute(0x1235, kServer);
	routes.route.kadPort = 4672;
	routes.serverCandidate = ServerRoute(0x2345, kServer + 1);
	routes.lastResortIPv4.id = kHighID;
	routes.lastResortIPv4.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	routes.lastResortIPv4.tcpPort = 4662;
	return routes;
}

// The routes once the fallback server route is in use.
SSourceRoutes OnServerCandidate(const SSourceRoutes &start)
{
	SSourceRoutes routes = start;
	routes.route.id = start.serverCandidate.id;
	routes.route.serverIP = start.serverCandidate.serverIP;
	routes.serverCandidate = SStoredSourceRoute();
	return routes;
}

bool SameRoutes(const SSourceRoutes &a, const SSourceRoutes &b)
{
	return a.route == b.route && a.serverCandidate == b.serverCandidate &&
	       a.lastResortIPv4 == b.lastResortIPv4 && a.lastResortIPv6 == b.lastResortIPv6 &&
	       a.identityFromFallback == b.identityFromFallback && a.routeUnproven == b.routeUnproven;
}
} // namespace

TEST(CSourceRouteChainStart, OneFallbackGivesBackTheStart)
{
	CSourceRouteChainStart chain;
	const SSourceRoutes start = ChainStartRoutes();
	chain.Save(start, 2);

	const std::optional<CSourceRouteChainStart::SStart> taken = chain.Take();
	ASSERT_TRUE(taken.has_value());
	ASSERT_TRUE(SameRoutes(start, taken->routes));
	ASSERT_EQUALS(2u, taken->formerKeys);
	ASSERT_FALSE(chain.Take().has_value());
}

TEST(CSourceRouteChainStart, ASecondFallbackKeepsTheFirstStart)
{
	CSourceRouteChainStart chain;
	const SSourceRoutes start = ChainStartRoutes();
	chain.Save(start, 1);
	SSourceRoutes second = OnServerCandidate(start);
	second.identityFromFallback = true;
	chain.Save(second, 2);

	const std::optional<CSourceRouteChainStart::SStart> taken = chain.Take();
	ASSERT_TRUE(SameRoutes(start, taken->routes));
	ASSERT_EQUALS(1u, taken->formerKeys);
}

TEST(CSourceRouteChainStart, AnUpdateReplacesTheSavedRoutesOnly)
{
	CSourceRouteChainStart chain;
	const SSourceRoutes start = ChainStartRoutes();
	chain.Save(start, 2);
	SSourceRoutes updated = start;
	updated.route.id = 0x1236;
	chain.Update(updated);

	const std::optional<CSourceRouteChainStart::SStart> taken = chain.Take();
	ASSERT_TRUE(SameRoutes(updated, taken->routes));
	ASSERT_EQUALS(2u, taken->formerKeys);
}

TEST(CSourceRouteChainStart, NoChainNoUpdate)
{
	CSourceRouteChainStart chain;
	ASSERT_FALSE(chain.Routes().has_value());
	chain.Update(ChainStartRoutes());
	ASSERT_FALSE(chain.Take().has_value());

	chain.Save(ChainStartRoutes(), 0);
	ASSERT_TRUE(chain.Routes().has_value());
	chain.Reset();
	ASSERT_FALSE(chain.Take().has_value());
}

DECLARE_SIMPLE(CSourceRouteMerge)

namespace
{
SSourceRouteMergeFacts FromOurServer()
{
	SSourceRouteMergeFacts facts;
	facts.incomingTrusted = true;
	return facts;
}

void Merge(SSourceRoutes &routes, const SSourceRouteMergeFacts &facts, const SStoredSourceRoute &incoming)
{
	MergeSourceRoutes(routes, facts, incoming);
}

// 192.0.2.0 as an ed2k ID: below 0x1000000, so it reads as a LowID.
const uint32 kDotZeroIP = 0x000200C0u;
const uint32 kDotZeroHybrid = 0xC0000200u;

// Our server lists the source at kDotZeroIP, port 21338, as a server record does.
SStoredSourceRoute DotZeroServerRecord()
{
	SStoredSourceRoute record = ServerRoute(kDotZeroIP, kServer);
	record.tcpPort = 21338;
	return record;
}

SSourceRouteMergeFacts ServerLowIDFacts(bool trusted)
{
	SSourceRouteMergeFacts facts;
	facts.incomingTrusted = trusted;
	facts.incomingServerLowID = true;
	return facts;
}
} // namespace

TEST(CSourceRouteMerge, OurServerChangesTheLowIDOnTheStartAndOnTheFallback)
{
	const SSourceRoutes start = ChainStartRoutes();
	SSourceRouteMergeFacts facts = FromOurServer();
	facts.incomingServerLowID = true;
	const SStoredSourceRoute incoming = ServerRoute(0x1236, kServer);

	SSourceRoutes onStart = start;
	Merge(onStart, facts, incoming);
	ASSERT_EQUALS(0x1236u, onStart.route.id);
	ASSERT_EQUALS(kServer, onStart.route.serverIP);
	ASSERT_TRUE(ServerRouteOf(start.route) == onStart.serverCandidate);

	SSourceRoutes onFallback = OnServerCandidate(start);
	Merge(onFallback, facts, incoming);
	ASSERT_EQUALS(0x1236u, onFallback.route.id);
	ASSERT_EQUALS(kServer, onFallback.route.serverIP);
}

TEST(CSourceRouteMerge, AnIPv6IdentityOnTheStartIsNotReplacedByAHighID)
{
	// An IPv6-only source that moved to its last-resort IPv4 when our server lists it at another
	// HighID.
	SSourceRoutes start;
	start.route.id = 1;
	start.route.address = IPv6(7);
	start.route.tcpPort = 4662;
	start.lastResortIPv4.id = kHighID;
	start.lastResortIPv4.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	start.lastResortIPv4.tcpPort = 4662;
	SSourceRoutes onFallback = start;
	onFallback.route = start.lastResortIPv4;
	onFallback.lastResortIPv4 = SStoredSourceRoute();
	onFallback.identityFromFallback = true;

	SSourceRouteMergeFacts facts = FromOurServer();
	facts.incomingHighIDIPv4 = true;
	SStoredSourceRoute incoming;
	incoming.id = kHighID + 1;
	incoming.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID + 1);
	incoming.tcpPort = 4662;

	Merge(onFallback, facts, incoming);
	ASSERT_TRUE(incoming.address == onFallback.route.address);
	ASSERT_FALSE(onFallback.identityFromFallback);

	SSourceRoutes onStart = start;
	Merge(onStart, facts, incoming);
	ASSERT_TRUE(SameRoutes(start, onStart));
}

TEST(CSourceRouteMerge, AnIPv6IdentityStoresTheHighIDAsItsLastResort)
{
	SSourceRoutes start;
	start.route.id = 1;
	start.route.address = IPv6(7);
	start.route.tcpPort = 4662;
	SSourceRouteMergeFacts facts = FromOurServer();
	facts.incomingHighIDIPv4 = true;
	SStoredSourceRoute incoming;
	incoming.id = kHighID;
	incoming.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	incoming.tcpPort = 4662;

	Merge(start, facts, incoming);
	ASSERT_TRUE(IPv6(7) == start.route.address);
	ASSERT_TRUE(incoming == start.lastResortIPv4);
}

TEST(CSourceRouteMerge, AnIPv6IdentityTakesNoAdvertisedIPv6ItsFallbackWould)
{
	// An IPv6-only source on its last-resort IPv4 gets a Kad copy with another IPv6.
	SSourceRoutes start;
	start.route.id = 1;
	start.route.address = IPv6(7);
	start.route.tcpPort = 4662;
	SSourceRoutes onFallback = start;
	onFallback.route.id = kHighID;
	onFallback.route.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	SSourceRouteMergeFacts facts;
	facts.incomingRoutableIPv6 = true;
	SStoredSourceRoute incoming;
	incoming.id = 1;
	incoming.address = IPv6(8);
	incoming.tcpPort = 4662;

	ASSERT_TRUE(DecideSourceRouteMerge(WithRouteFacts(facts, onFallback, incoming)).takeAdvertisedIPv6);
	const SSourceRouteMerge onStart = DecideSourceRouteMerge(WithRouteFacts(facts, start, incoming));
	ASSERT_FALSE(onStart.takeAdvertisedIPv6);
	ASSERT_FALSE(onStart.takeLastResortIPv6);
}

TEST(CSourceRouteMerge, AnIPv4StartTakesTheAdvertisedIPv6ItsIPv6FallbackWouldNot)
{
	// An IPv4 LowID on its last-resort IPv6 gets a Kad copy with an IPv6.
	SSourceRoutes start;
	start.route.id = 0x1235;
	start.route.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	start.route.serverIP = kServer;
	start.route.serverPort = 4661;
	start.route.tcpPort = 4662;
	SSourceRoutes onFallback = start;
	onFallback.route.address = IPv6(7);
	SSourceRouteMergeFacts facts;
	facts.incomingRoutableIPv6 = true;
	SStoredSourceRoute incoming;
	incoming.id = 1;
	incoming.address = IPv6(8);
	incoming.tcpPort = 4662;

	ASSERT_FALSE(DecideSourceRouteMerge(WithRouteFacts(facts, onFallback, incoming)).takeAdvertisedIPv6);
	ASSERT_TRUE(DecideSourceRouteMerge(WithRouteFacts(facts, start, incoming)).takeAdvertisedIPv6);
}

TEST(CSourceRouteMerge, AHighIDAtADotZeroIPListedByOurServerStaysAHighID)
{
	SSourceRoutes known;
	known.route.id = kDotZeroHybrid;
	known.route.address = CNetworkAddress::FromIPv4NetworkOrder(kDotZeroIP);
	known.route.tcpPort = 21338;

	SSourceRoutes merged = known;
	const SSourceRouteMerge merge =
		MergeSourceRoutes(merged, ServerLowIDFacts(true), DotZeroServerRecord());
	ASSERT_FALSE(merge.takeServerRoute);
	ASSERT_TRUE(SameRoutes(known, merged));
}

TEST(CSourceRouteMerge, ALowIDListedByOurServerAtItsOwnDotZeroIPBecameAHighID)
{
	SSourceRoutes known;
	known.route = ServerRoute(0x1235, kServer);
	known.route.address = CNetworkAddress::FromIPv4NetworkOrder(kDotZeroIP);
	known.route.tcpPort = 21338;

	SSourceRoutes merged = known;
	MergeSourceRoutes(merged, ServerLowIDFacts(true), DotZeroServerRecord());
	ASSERT_EQUALS(kDotZeroHybrid, merged.route.id);
	ASSERT_TRUE(known.route.address == merged.route.address);
	ASSERT_EQUALS(0x1235u, merged.serverCandidate.id);

	SSourceRoutes untrusted = known;
	MergeSourceRoutes(untrusted, ServerLowIDFacts(false), DotZeroServerRecord());
	ASSERT_EQUALS(0x1235u, untrusted.route.id);
	ASSERT_EQUALS(kDotZeroHybrid, untrusted.lastResortIPv4.id);
}

TEST(CSourceRouteMerge, ADotZeroRecordIsALowIDWithoutAKnownAddress)
{
	SSourceRoutes known;
	known.route = ServerRoute(0x1235, kServer);
	SSourceRoutes merged = known;
	MergeSourceRoutes(merged, ServerLowIDFacts(true), DotZeroServerRecord());
	ASSERT_EQUALS(kDotZeroIP, merged.route.id);
}

TEST(CSourceRouteMerge, ADotZeroHighIDOnANewPortKeepsItsDial)
{
	SSourceRoutes known;
	known.route.id = kDotZeroHybrid;
	known.route.address = CNetworkAddress::FromIPv4NetworkOrder(kDotZeroIP);
	known.route.tcpPort = 8249;

	SSourceRoutes merged = known;
	const SSourceRouteMerge merge =
		MergeSourceRoutes(merged, ServerLowIDFacts(true), DotZeroServerRecord());
	ASSERT_FALSE(merge.takeServerRoute);
	ASSERT_EQUALS(kDotZeroHybrid, merged.route.id);
	ASSERT_EQUALS(21338, merged.route.tcpPort);
	ASSERT_EQUALS(8249, merged.lastResortIPv4.tcpPort);
}

TEST(CSourceRouteMerge, AChainOnAnIPv6FallbackReadsADotZeroRecordByItsStart)
{
	SSourceRoutes start;
	start.route.id = kDotZeroHybrid;
	start.route.address = CNetworkAddress::FromIPv4NetworkOrder(kDotZeroIP);
	start.route.tcpPort = 21338;
	start.lastResortIPv6.address = IPv6(7);
	start.lastResortIPv6.tcpPort = 21338;
	SSourceRoutes onFallback = start;
	onFallback.route.address = IPv6(7);
	onFallback.lastResortIPv6 = SStoredSourceRoute();

	const SSourceRouteMerge merge =
		MergeSourceRoutes(onFallback, ServerLowIDFacts(true), DotZeroServerRecord(), &start);
	ASSERT_FALSE(merge.takeServerRoute);
	ASSERT_TRUE(IPv6(7) == onFallback.route.address);
}

TEST(CSourceRouteMerge, AServerRouteFromAnotherChannelIsUnproven)
{
	// A Kad LowID: ID 1, no server.
	SSourceRoutes known;
	known.route.id = 1;
	known.route.tcpPort = 4662;
	SSourceRouteMergeFacts facts;
	facts.incomingServerLowID = true;
	const SSourceRouteMerge merge = MergeSourceRoutes(known, facts, ServerRoute(0x1235, kServer));
	ASSERT_TRUE(merge.takeServerRoute);
	ASSERT_TRUE(known.routeUnproven);
}

TEST(CSourceRouteMerge, ADirectCallbackFromAnotherChannelIsUnproven)
{
	SSourceRoutes known;
	known.route = ServerRoute(0x1235, kServer);
	SSourceRouteMergeFacts facts;
	facts.incomingDirectCallback = true;
	SStoredSourceRoute incoming;
	incoming.id = 1;
	incoming.address = CNetworkAddress::FromIPv4NetworkOrder(kHighID);
	incoming.kadPort = 4672;
	const SSourceRouteMerge merge = MergeSourceRoutes(known, facts, incoming);
	ASSERT_TRUE(merge.takeDirectCallback);
	ASSERT_TRUE(known.routeUnproven);
}

// A hashless ExtSX copy matched by its buddy ID: its server route is only a fallback when the
// source already has one, and its route stays the same.
TEST(CSourceRouteMerge, AnotherChannelsServerRouteIsOnlyAFallbackBesideTheSourcesOwn)
{
	SSourceRoutes known;
	known.route = ServerRoute(0x1235, kServer);
	SSourceRouteMergeFacts facts;
	facts.incomingServerLowID = true;
	const SSourceRouteMerge merge = MergeSourceRoutes(known, facts, ServerRoute(0x1236, kServer + 1));
	ASSERT_FALSE(merge.takeServerRoute);
	ASSERT_TRUE(merge.storeServerCandidate);
	ASSERT_TRUE(ServerRoute(0x1235, kServer) == known.route);
	ASSERT_TRUE(ServerRoute(0x1236, kServer + 1) == known.serverCandidate);
	ASSERT_FALSE(known.routeUnproven);
}

TEST(CSourceRouteMerge, ARouteFromOurServerLeavesTheFlag)
{
	SSourceRoutes known;
	known.route = ServerRoute(0x1235, kServer);
	SSourceRouteMergeFacts facts = FromOurServer();
	facts.incomingServerLowID = true;
	const SSourceRouteMerge merge = MergeSourceRoutes(known, facts, ServerRoute(0x1236, kServer));
	ASSERT_TRUE(merge.takeServerRoute);
	ASSERT_FALSE(known.routeUnproven);

	known.routeUnproven = true;
	MergeSourceRoutes(known, facts, ServerRoute(0x1237, kServer));
	ASSERT_TRUE(known.routeUnproven);
}

TEST(CSourceRouteChainStart, TheChainStartKeepsTheUnprovenFlag)
{
	CSourceRouteChainStart chain;
	SSourceRoutes start = ChainStartRoutes();
	start.routeUnproven = true;
	chain.Save(start, 0);
	ASSERT_TRUE(chain.Take()->routes.routeUnproven);
}
