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

TEST(CDeadSourceKey, OnlyLegacyLowIDUsesFirewalledBlockTime)
{
	const auto native = Key(CProtocolPeerIdentity::FromNativeIPv6(IPv6(1)), 0, 4662, 0, 4672);
	ASSERT_FALSE(native.UsesFirewalledBlockTime());

	const auto high = CProtocolPeerIdentity::FromIPv4HighID(0xC0000201);
	ASSERT_FALSE(Key(high, 0xC0000201, 4662, 0, 0).UsesFirewalledBlockTime());

	const auto low = CProtocolPeerIdentity::FromServerScopedLowID(
		42, CNetworkAddress::FromIPv4NetworkOrder(0x0100000a), 4662);
	ASSERT_TRUE(Key(low, 42, 4662, 0x0a000001, 0).UsesFirewalledBlockTime());
	ASSERT_TRUE(Key(CProtocolPeerIdentity::Absent(), 0, 4662, 0, 0).UsesFirewalledBlockTime());
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
