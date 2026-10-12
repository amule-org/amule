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
#include "KadSourceAddressPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadSourceAddressPolicy)

namespace
{
// Every switch an IPv6 contact needs is on, so each test turns off exactly one.
SKadSourceFacts ContactableOverIPv6()
{
	SKadSourceFacts facts;
	facts.sourceType = 3;
	facts.publishedIPv6 = CNetworkAddress::FromString("2001:4860:4860::8888");
	facts.ourHelloID = 1;
	facts.ipv6Built = true;
	facts.familyPermitted = true;
	return facts;
}
} // namespace

TEST(KadSourceAddressPolicy, AFirewalledSourceIsContactedOnItsPublishedIPv6)
{
	for (std::uint8_t sourceType : { 3, 5, 6 }) {
		SKadSourceFacts facts = ContactableOverIPv6();
		facts.sourceType = sourceType;
		ASSERT_TRUE(ShouldContactKadSourceOverIPv6(facts));
	}
}

TEST(KadSourceAddressPolicy, AnyNonzeroLowIDOfOursQualifies)
{
	for (std::uint32_t ourHelloID : { 1u, 0x00ABCDEFu }) {
		SKadSourceFacts facts = ContactableOverIPv6();
		facts.ourHelloID = ourHelloID;
		ASSERT_TRUE(ShouldContactKadSourceOverIPv6(facts));
	}
}

// Types 1 and 4 are HighID sources that keep their IPv4 route.
TEST(KadSourceAddressPolicy, OtherSourceTypesAreNotContactedOverIPv6)
{
	for (std::uint8_t sourceType : { 0, 1, 2, 4, 7 }) {
		SKadSourceFacts facts = ContactableOverIPv6();
		facts.sourceType = sourceType;
		ASSERT_FALSE(ShouldContactKadSourceOverIPv6(facts));
	}
}

TEST(KadSourceAddressPolicy, AZeroOrHighIDOfOursIsNotContactedOverIPv6)
{
	for (std::uint32_t ourHelloID : { 0u, 16777216u, 0xC0A80001u }) {
		SKadSourceFacts facts = ContactableOverIPv6();
		facts.ourHelloID = ourHelloID;
		ASSERT_FALSE(ShouldContactKadSourceOverIPv6(facts));
	}
}

TEST(KadSourceAddressPolicy, APublishedAddressThatNamesNoRemotePeerIsNotContacted)
{
	const CNetworkAddress mapped =
		CNetworkAddress::IPv6FromOctets({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 203, 0, 113, 5 });
	for (const CNetworkAddress &address : { CNetworkAddress::FromString("203.0.113.5"),
		     mapped,
		     CNetworkAddress::FromString("::1"),
		     CNetworkAddress::FromString("fe80::1"),
		     CNetworkAddress::FromString("fd00::1"),
		     CNetworkAddress::FromString("2001:db8::1") }) {
		ASSERT_TRUE(address.IsPresent());
		SKadSourceFacts facts = ContactableOverIPv6();
		facts.publishedIPv6 = address;
		ASSERT_FALSE(ShouldContactKadSourceOverIPv6(facts));
	}

	SKadSourceFacts absent = ContactableOverIPv6();
	absent.publishedIPv6 = CNetworkAddress::Absent();
	ASSERT_FALSE(ShouldContactKadSourceOverIPv6(absent));
}

TEST(KadSourceAddressPolicy, AFilteredPublishedAddressIsNotContacted)
{
	SKadSourceFacts facts = ContactableOverIPv6();
	facts.publishedIPv6Filtered = true;
	ASSERT_FALSE(ShouldContactKadSourceOverIPv6(facts));
}

TEST(KadSourceAddressPolicy, EachMissingDialPermissionRefusesTheIPv6Contact)
{
	SKadSourceFacts notBuilt = ContactableOverIPv6();
	notBuilt.ipv6Built = false;
	ASSERT_FALSE(ShouldContactKadSourceOverIPv6(notBuilt));

	SKadSourceFacts notPermitted = ContactableOverIPv6();
	notPermitted.familyPermitted = false;
	ASSERT_FALSE(ShouldContactKadSourceOverIPv6(notPermitted));

	SKadSourceFacts proxied = ContactableOverIPv6();
	proxied.proxyEnabled = true;
	ASSERT_FALSE(ShouldContactKadSourceOverIPv6(proxied));
}
