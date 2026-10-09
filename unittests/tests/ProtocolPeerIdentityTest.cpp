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
#include "ProtocolPeerIdentity.h"
using namespace muleunit;
DECLARE_SIMPLE(CProtocolPeerIdentity)

namespace
{
CNetworkAddress IPv6(unsigned long scope = 0)
{
	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 1;
	bytes[15] = 1;
	return CNetworkAddress::IPv6FromOctets(bytes, scope);
}
} // namespace

TEST(CProtocolPeerIdentity, HighIDUsesHostOrderIPv4Semantics)
{
	const auto expected = CNetworkAddress::Octets{ 192, 0, 2, 1 };
	const auto high = CProtocolPeerIdentity::FromIPv4HighID(0xC0000201);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::IPv4HighID == high.GetKind());
	ASSERT_TRUE(expected == high.Address().GetOctets());

	const auto fromClient =
		CProtocolPeerIdentity::FromClientState(0xC0000201, false, CNetworkAddress::Absent(), 0, 0);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::IPv4HighID == fromClient.GetKind());
	ASSERT_TRUE(expected == fromClient.Address().GetOctets());
}

TEST(CProtocolPeerIdentity, HighLowAndNativeSemantics)
{
	const auto high = CProtocolPeerIdentity::FromIPv4HighID(0x01020304);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::IPv4HighID == high.GetKind());
	ASSERT_TRUE((CNetworkAddress::Octets{ 1, 2, 3, 4 }) == high.Address().GetOctets());
	ASSERT_FALSE(CProtocolPeerIdentity::FromIPv4HighID(0).TryGetIPv4HighID());
	ASSERT_FALSE(CProtocolPeerIdentity::FromIPv4HighID(42).TryGetIPv4HighID());
	ASSERT_TRUE(
		CProtocolPeerIdentity::Kind::Absent == CProtocolPeerIdentity::FromIPv4HighID(0).GetKind());
	ASSERT_TRUE(
		CProtocolPeerIdentity::Kind::Absent == CProtocolPeerIdentity::FromIPv4HighID(42).GetKind());

	const auto server = CNetworkAddress::FromIPv4NetworkOrder(0x0100000a);
	const auto low = CProtocolPeerIdentity::FromServerScopedLowID(42, server, 4662);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::ServerScopedLowID == low.GetKind());
	ASSERT_EQUALS(uint32_t(42), *low.TryGetServerScopedLowID());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromServerScopedLowID(0, server, 4662).GetKind());
	ASSERT_TRUE(
		CProtocolPeerIdentity::Kind::Absent ==
		CProtocolPeerIdentity::FromServerScopedLowID(42, CNetworkAddress::Absent(), 4662).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromServerScopedLowID(42, server, 0).GetKind());
	ASSERT_TRUE(low != CProtocolPeerIdentity::FromServerScopedLowID(
				   42, CNetworkAddress::FromIPv4NetworkOrder(0x0200000a), 4662));

	CNetworkAddress::Octets mappedBytes{};
	mappedBytes[10] = 0xff;
	mappedBytes[11] = 0xff;
	mappedBytes[12] = 10;
	mappedBytes[15] = 1;
	const auto mapped = CNetworkAddress::IPv6FromOctets(mappedBytes);
	ASSERT_TRUE(CProtocolPeerIdentity::FromServerScopedLowID(42, mapped, 4662) == low);

	const auto native = CProtocolPeerIdentity::FromNativeIPv6(IPv6());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::NativeIPv6 == native.GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromNativeIPv6(CNetworkAddress::Absent()).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromNativeIPv6(CNetworkAddress::AnyIPv6()).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromNativeIPv6(server).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromNativeIPv6(mapped).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::NativeIPv6 ==
		    CProtocolPeerIdentity::FromClientState(42, false, IPv6(), 0x0a000001, 4662).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::NativeIPv6 ==
		    CProtocolPeerIdentity::FromClientState(42, true, IPv6(), 0x0a000001, 4662).GetKind());
	ASSERT_TRUE(
		CProtocolPeerIdentity::Kind::ServerScopedLowID ==
		CProtocolPeerIdentity::FromClientState(42, true, CNetworkAddress::Absent(), 0x0a000001, 4662)
			.GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromClientState(42, true, CNetworkAddress::Absent(), 0, 4662)
			    .GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromClientState(42, true, CNetworkAddress::Absent(), 0x0a000001, 0)
			    .GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::IPv4HighID ==
		    CProtocolPeerIdentity::FromClientState(0x01020304, false, CNetworkAddress::Absent(), 0, 0)
			    .GetKind());
	ASSERT_TRUE(native != CProtocolPeerIdentity::FromNativeIPv6(IPv6(7)));
}

TEST(CProtocolPeerIdentity, OrderingIsStrict)
{
	const auto absent = CProtocolPeerIdentity::Absent();
	const auto one = CProtocolPeerIdentity::FromIPv4HighID(0x01000000);
	const auto two = CProtocolPeerIdentity::FromIPv4HighID(0x01000001);
	ASSERT_TRUE(absent < one);
	ASSERT_TRUE(one < two);
	ASSERT_FALSE(one < one);
	ASSERT_TRUE(one == CProtocolPeerIdentity::FromIPv4HighID(0x01000000));
	ASSERT_TRUE(one != CProtocolPeerIdentity::FromIPv4HighID(0x01000001));

	const auto scoped = CProtocolPeerIdentity::FromNativeIPv6(IPv6(1));
	const auto differentlyScoped = CProtocolPeerIdentity::FromNativeIPv6(IPv6(2));
	ASSERT_TRUE(scoped != differentlyScoped);
	ASSERT_TRUE(scoped < differentlyScoped || differentlyScoped < scoped);
	ASSERT_FALSE(scoped < differentlyScoped && differentlyScoped < scoped);
}
