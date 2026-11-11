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
