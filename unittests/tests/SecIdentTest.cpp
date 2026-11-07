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

#include "SecIdentPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(SecIdent)

TEST(SecIdent, AdvertisementRequiresCryptoAndIPv4ForV2)
{
	ASSERT_EQUALS(0u, SecIdent::SupportedVersions(false, false));
	ASSERT_EQUALS(0u, SecIdent::SupportedVersions(false, true));
	ASSERT_EQUALS(1u, SecIdent::SupportedVersions(true, false));
	ASSERT_EQUALS(3u, SecIdent::SupportedVersions(true, true));
}

TEST(SecIdent, IPv4KeepsLegacyVersionPreference)
{
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0, true));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(1, true));
	ASSERT_EQUALS(SecIdent::V2, SecIdent::SignatureVersion(2, true));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(3, true));
}

TEST(SecIdent, WithoutIPv4OnlyMutuallySupportedV1IsUsable)
{
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0, false));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(1, false));
	// A fallback must not invent v1 support for a peer advertising only v2.
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(2, false));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(3, false));
}

TEST(SecIdent, ReservedFeatureBitsDoNotEnableSecureIdentification)
{
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0xfc, true));
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0xfc, false));
	ASSERT_EQUALS(SecIdent::V1, SecIdent::SignatureVersion(0xfd, false));
	ASSERT_EQUALS(SecIdent::V2, SecIdent::SignatureVersion(0xfe, true));
	ASSERT_EQUALS(SecIdent::Unavailable, SecIdent::SignatureVersion(0xfe, false));
}

// Exercise the exact prerequisite used by VerifyIdent before crypto or credit-state
// mutation. This is policy coverage, not an RSA/application integration test.
TEST(SecIdent, VerifyIdentV2RefusesEndpointsWithoutNonzeroIPv4)
{
	uint32_t ip = 123;
	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 0x01;
	bytes[15] = 1;
	ASSERT_FALSE(SecIdent::PeerIPv4(CNetworkAddress::IPv6FromOctets(bytes), ip));
	ASSERT_EQUALS(uint32_t(0), ip);
	ip = 123;
	ASSERT_FALSE(SecIdent::PeerIPv4(CNetworkAddress::Absent(), ip));
	ASSERT_EQUALS(uint32_t(0), ip);
	ASSERT_FALSE(SecIdent::PeerIPv4(CNetworkAddress::FromIPv4NetworkOrder(0), ip));
	ASSERT_EQUALS(uint32_t(0), ip);
	bytes = {};
	bytes[10] = bytes[11] = 0xff;
	ASSERT_FALSE(SecIdent::PeerIPv4(CNetworkAddress::IPv6FromOctets(bytes), ip));
	ASSERT_EQUALS(uint32_t(0), ip);
}

TEST(SecIdent, VerifyIdentV2DerivesIdenticalMappedAndPlainChallengeIP)
{
	const uint32_t expected = 0x010200c0;
	uint32_t plain = 0;
	ASSERT_TRUE(SecIdent::PeerIPv4(CNetworkAddress::FromIPv4NetworkOrder(expected), plain));
	ASSERT_EQUALS(expected, plain);
	CNetworkAddress::Octets bytes{};
	bytes[10] = bytes[11] = 0xff;
	bytes[12] = 192;
	bytes[14] = 2;
	bytes[15] = 1;
	uint32_t mapped = 0;
	ASSERT_TRUE(SecIdent::PeerIPv4(CNetworkAddress::IPv6FromOctets(bytes), mapped));
	ASSERT_EQUALS(plain, mapped);
}

TEST(SecIdent, RemoteClientChallengeIPIsTheAddressThePeerSeesUsAt)
{
	const uint32_t login = 0x017100cb;  // 203.0.113.1
	const uint32_t udp = 0x027100cb;    // 203.0.113.2
	const uint32_t highId = 0x0a7100cb; // a HighID is our IPv4
	// Over a split NAT the peer saw our UDP address.
	ASSERT_EQUALS(udp, SecIdent::RemoteClientChallengeIP(udp, login, login, true, 5));
	// Otherwise the historical choice, unchanged.
	ASSERT_EQUALS(login, SecIdent::RemoteClientChallengeIP(0, login, login, true, 5));
	ASSERT_EQUALS(login, SecIdent::RemoteClientChallengeIP(0, 0, login, true, 5));
	ASSERT_EQUALS(highId, SecIdent::RemoteClientChallengeIP(0, 0, login, false, highId));
}

// File_checked_for_headers
