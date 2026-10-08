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
#include <muleunit/test.h>
#include "ProtocolPeerIdentity.h"
using namespace muleunit;
DECLARE_SIMPLE(CProtocolPeerIdentity)

TEST(CProtocolPeerIdentity, FormsSelectionAndOrdering)
{
	const auto absent = CProtocolPeerIdentity::Absent();
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent == absent.GetKind());
	const auto legacy = CProtocolPeerIdentity::FromLegacyIPv4Id(42);
	ASSERT_EQUALS(uint32_t(42), *legacy.TryGetLegacyIPv4Id());
	ASSERT_FALSE(absent.TryGetLegacyIPv4Id());
	const auto zeroLegacy = CProtocolPeerIdentity::FromLegacyIPv4Id(0);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent == zeroLegacy.GetKind());
	ASSERT_FALSE(zeroLegacy.TryGetLegacyIPv4Id());

	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 1;
	bytes[15] = 1;
	const auto ipv6 = CNetworkAddress::IPv6FromOctets(bytes);
	const auto native = CProtocolPeerIdentity::FromNativeIPv6(ipv6);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::NativeIPv6 == native.GetKind());
	ASSERT_TRUE(ipv6 == native.NativeIPv6());
	ASSERT_TRUE(
		CProtocolPeerIdentity::Kind::Absent ==
		CProtocolPeerIdentity::FromNativeIPv6(CNetworkAddress::FromIPv4NetworkOrder(1)).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent ==
		    CProtocolPeerIdentity::FromNativeIPv6(CNetworkAddress::AnyIPv6()).GetKind());

	ASSERT_TRUE(absent < legacy);
	ASSERT_TRUE(CProtocolPeerIdentity::FromLegacyIPv4Id(1) < legacy);
	ASSERT_TRUE(legacy != native);
	ASSERT_TRUE(legacy == CProtocolPeerIdentity::FromLegacyIPv4Id(42));
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::LegacyIPv4Id ==
		    CProtocolPeerIdentity::FromLegacyIPv4IdOrNativeIPv6(9, ipv6).GetKind());
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::NativeIPv6 ==
		    CProtocolPeerIdentity::FromLegacyIPv4IdOrNativeIPv6(0, ipv6).GetKind());
	ASSERT_TRUE(
		CProtocolPeerIdentity::Kind::Absent ==
		CProtocolPeerIdentity::FromLegacyIPv4IdOrNativeIPv6(0, CNetworkAddress::Absent()).GetKind());

	CNetworkAddress::Octets mappedBytes{};
	mappedBytes[10] = 0xff;
	mappedBytes[11] = 0xff;
	mappedBytes[12] = 192;
	mappedBytes[13] = 0;
	mappedBytes[14] = 2;
	mappedBytes[15] = 1;
	const auto mapped = CNetworkAddress::IPv6FromOctets(mappedBytes);
	const auto mappedIdentity = CProtocolPeerIdentity::FromNativeIPv6(mapped);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent == mappedIdentity.GetKind());
	ASSERT_FALSE(mappedIdentity.TryGetLegacyIPv4Id());
	const auto mappedLegacyIdentity = CProtocolPeerIdentity::FromLegacyIPv4IdOrNativeIPv6(0, mapped);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::Absent == mappedLegacyIdentity.GetKind());
	ASSERT_FALSE(mappedLegacyIdentity.TryGetLegacyIPv4Id());

	CNetworkAddress::Octets loopbackBytes{};
	loopbackBytes[15] = 1;
	const auto loopback = CNetworkAddress::IPv6FromOctets(loopbackBytes);
	const auto nativeLoopback = CProtocolPeerIdentity::FromNativeIPv6(loopback);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::NativeIPv6 == nativeLoopback.GetKind());
	ASSERT_FALSE(nativeLoopback.TryGetLegacyIPv4Id());
	ASSERT_TRUE(nativeLoopback != native);
	ASSERT_TRUE(nativeLoopback < native || native < nativeLoopback);
	ASSERT_FALSE(nativeLoopback < nativeLoopback);
	ASSERT_FALSE(native < native);
	ASSERT_TRUE(nativeLoopback == CProtocolPeerIdentity::FromNativeIPv6(loopback));
	const auto selected = CProtocolPeerIdentity::FromLegacyIPv4IdOrNativeIPv6(9, ipv6);
	ASSERT_TRUE(CProtocolPeerIdentity::Kind::LegacyIPv4Id == selected.GetKind());
	ASSERT_EQUALS(uint32_t(9), *selected.TryGetLegacyIPv4Id());
	ASSERT_TRUE(selected != native);
}
