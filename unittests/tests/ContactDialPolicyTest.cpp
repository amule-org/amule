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
#include "ContactDialPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(ContactDialPolicy)

namespace
{
CNetworkAddress NativeIPv6()
{
	return CNetworkAddress::IPv6FromOctets(
		{ 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 });
}

CNetworkAddress PeerIPv4()
{
	return CNetworkAddress::FromString("192.0.2.1");
}

CNetworkAddress PeerIPv6()
{
	return CNetworkAddress::FromString("2001:4860:4860::8888");
}

CNetworkAddress MappedPeerIPv4()
{
	return CNetworkAddress::FromString("::ffff:192.0.2.1");
}

// Every switch a native IPv6 dial needs is on, so each test turns off exactly one.
SContactFacts DialableIPv6()
{
	SContactFacts facts;
	facts.contactAddress = NativeIPv6();
	facts.ipv6Built = true;
	facts.familyPermitted = true;
	return facts;
}
} // namespace

// A promoted dial must clear the filter and the ban list on both addresses of the same host.
TEST(ContactDialPolicy, APromotedDialChecksTheTargetAndTheRetainedIPv4)
{
	const std::vector<CNetworkAddress> checked = ContactSecurityAddresses(PeerIPv6(), PeerIPv4());
	ASSERT_EQUALS(2u, (unsigned)checked.size());
	ASSERT_TRUE(checked[0] == PeerIPv6());
	ASSERT_TRUE(checked[1] == PeerIPv4());
}

// Every other route checks exactly the one address it checked before promotion existed.
TEST(ContactDialPolicy, OtherContactsCheckOnlyTheirTarget)
{
	const std::vector<CNetworkAddress> ipv4 = ContactSecurityAddresses(PeerIPv4(), PeerIPv4());
	ASSERT_EQUALS(1u, (unsigned)ipv4.size());
	ASSERT_TRUE(ipv4[0] == PeerIPv4());

	const std::vector<CNetworkAddress> native = ContactSecurityAddresses(PeerIPv6(), PeerIPv6());
	ASSERT_EQUALS(1u, (unsigned)native.size());
	ASSERT_TRUE(native[0] == PeerIPv6());

	const std::vector<CNetworkAddress> noIdentity =
		ContactSecurityAddresses(PeerIPv4(), CNetworkAddress::Absent());
	ASSERT_EQUALS(1u, (unsigned)noIdentity.size());
	ASSERT_TRUE(noIdentity[0] == PeerIPv4());
}

TEST(ContactDialPolicy, OnlyAnIPv6DialToAnIPv4PeerIsAPromotion)
{
	ASSERT_TRUE(IsPromotedIPv6Dial(PeerIPv6(), PeerIPv4()));
	ASSERT_FALSE(IsPromotedIPv6Dial(PeerIPv6(), PeerIPv6()));
	ASSERT_FALSE(IsPromotedIPv6Dial(PeerIPv4(), PeerIPv4()));
	ASSERT_FALSE(IsPromotedIPv6Dial(MappedPeerIPv4(), PeerIPv4()));
	ASSERT_FALSE(IsPromotedIPv6Dial(CNetworkAddress::Absent(), PeerIPv4()));
}

TEST(ContactDialPolicy, AnIPv6DialTargetSkipsIPv4Utp)
{
	ASSERT_TRUE(ShouldTryIPv4Utp(PeerIPv4()));
	ASSERT_TRUE(ShouldTryIPv4Utp(MappedPeerIPv4()));
	ASSERT_FALSE(ShouldTryIPv4Utp(PeerIPv6()));
}

TEST(ContactDialPolicy, ANativeIPv6ContactIsDialledDirectly)
{
	ASSERT_TRUE(DecideContactRoute(DialableIPv6()) == EContactRoute::DialIPv6);
}

TEST(ContactDialPolicy, EachMissingPermissionRefusesTheNativeIPv6Dial)
{
	SContactFacts notBuilt = DialableIPv6();
	notBuilt.ipv6Built = false;
	ASSERT_TRUE(DecideContactRoute(notBuilt) == EContactRoute::Refuse);

	SContactFacts notPermitted = DialableIPv6();
	notPermitted.familyPermitted = false;
	ASSERT_TRUE(DecideContactRoute(notPermitted) == EContactRoute::Refuse);

	// The proxy protocols carry IPv4 only, so a direct IPv6 dial would bypass the proxy.
	SContactFacts proxied = DialableIPv6();
	proxied.proxyEnabled = true;
	ASSERT_TRUE(DecideContactRoute(proxied) == EContactRoute::Refuse);
}

TEST(ContactDialPolicy, AConnectedSocketIsReusedWhateverTheFamily)
{
	SContactFacts connected = DialableIPv6();
	connected.ipv6Built = false;
	connected.socketConnected = true;
	ASSERT_TRUE(DecideContactRoute(connected) == EContactRoute::Legacy);
}

// IPv4 and absent contacts keep today's path whatever the IPv6 switches say, so a build without
// ENABLE_IPV6 decides exactly as before.
TEST(ContactDialPolicy, IPv4AndAbsentContactsKeepTheLegacyPath)
{
	const CNetworkAddress mapped =
		CNetworkAddress::IPv6FromOctets({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1 });
	for (const CNetworkAddress &address :
		{ CNetworkAddress::FromString("192.0.2.1"), mapped, CNetworkAddress::Absent() }) {
		for (bool ipv6Built : { false, true }) {
			for (bool proxyEnabled : { false, true }) {
				SContactFacts facts;
				facts.contactAddress = address;
				facts.ipv6Built = ipv6Built;
				facts.familyPermitted = true;
				facts.proxyEnabled = proxyEnabled;
				ASSERT_TRUE(DecideContactRoute(facts) == EContactRoute::Legacy);
			}
		}
	}
}

// PeerAddressing::CanOpenConnection() refused these before this decision existed. The mapped zero
// address is IPv6 by family but has no IPv4 wire form, so it must not pass as native IPv6.
TEST(ContactDialPolicy, UnspecifiedContactsAreRefused)
{
	const CNetworkAddress mappedZero =
		CNetworkAddress::IPv6FromOctets({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0 });
	for (const CNetworkAddress &address :
		{ CNetworkAddress::FromIPv4HostOrder(0), CNetworkAddress::IPv6FromOctets({}), mappedZero }) {
		ASSERT_TRUE(address.IsPresent());
		SContactFacts facts = DialableIPv6();
		facts.contactAddress = address;
		ASSERT_TRUE(DecideContactRoute(facts) == EContactRoute::Refuse);
	}
}

TEST(ContactDialPolicy, GloballyRoutablePeerIPv6PromotesOnlyForAUsableDualStackLocalNode)
{
	const CNetworkAddress ipv4 = CNetworkAddress::FromString("192.0.2.1");
	const CNetworkAddress peer = CNetworkAddress::FromString("2001:4860:4860::8888");
	const CNetworkAddress local = CNetworkAddress::FromString("2606:4700::1111");

	ASSERT_TRUE(
		PromotePeerIPv6ForDial(ipv4, peer, CNetworkAddress::Absent(), true, local, false) == peer);
	ASSERT_TRUE(
		PromotePeerIPv6ForDial(ipv4, peer, CNetworkAddress::Absent(), false, local, false) == ipv4);
	ASSERT_TRUE(PromotePeerIPv6ForDial(
			    ipv4, peer, CNetworkAddress::Absent(), true, CNetworkAddress::Absent(), false) ==
		    ipv4);
	ASSERT_TRUE(PromotePeerIPv6ForDial(ipv4,
			    peer,
			    CNetworkAddress::Absent(),
			    true,
			    CNetworkAddress::FromString("fe80::1"),
			    false) == ipv4);
	ASSERT_TRUE(PromotePeerIPv6ForDial(ipv4, peer, CNetworkAddress::Absent(), true, local, true) == ipv4);
	ASSERT_TRUE(PromotePeerIPv6ForDial(ipv4,
			    CNetworkAddress::FromString("fe80::1"),
			    CNetworkAddress::Absent(),
			    true,
			    local,
			    false) == ipv4);
}

TEST(ContactDialPolicy, PeerIPv6PromotionDoesNotReplaceNativeIPv6OrRetryAFailedAddress)
{
	const CNetworkAddress peer = CNetworkAddress::FromString("2001:4860:4860::8888");
	const CNetworkAddress local = CNetworkAddress::FromString("2606:4700::1111");
	const CNetworkAddress native = CNetworkAddress::FromString("2001:db8::1");

	ASSERT_TRUE(PromotePeerIPv6ForDial(native, peer, CNetworkAddress::Absent(), true, local, false) ==
		    native);
	ASSERT_TRUE(PromotePeerIPv6ForDial(
			    CNetworkAddress::FromString("192.0.2.1"), peer, peer, true, local, false) ==
		    CNetworkAddress::FromString("192.0.2.1"));
}
