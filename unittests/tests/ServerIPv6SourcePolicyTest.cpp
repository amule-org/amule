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
#include "ServerIPv6SourcePolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(ServerIPv6SourcePolicy)

namespace
{
// Every switch an IPv6 contact needs is on, so each test turns off exactly one.
SServerIPv6SourceFacts ContactableOverIPv6()
{
	SServerIPv6SourceFacts facts;
	facts.address = CNetworkAddress::FromString("2001:4860:4860::8888");
	facts.ipv6Built = true;
	facts.familyPermitted = true;
	return facts;
}
} // namespace

TEST(ServerIPv6SourcePolicy, AUsableAddressIsContactedOverIPv6)
{
	ASSERT_TRUE(ShouldContactServerSourceOverIPv6(ContactableOverIPv6()));
}

TEST(ServerIPv6SourcePolicy, AnAddressThatNamesNoRemotePeerIsNotContacted)
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
		SServerIPv6SourceFacts facts = ContactableOverIPv6();
		facts.address = address;
		ASSERT_FALSE(ShouldContactServerSourceOverIPv6(facts));
	}

	SServerIPv6SourceFacts absent = ContactableOverIPv6();
	absent.address = CNetworkAddress::Absent();
	ASSERT_FALSE(ShouldContactServerSourceOverIPv6(absent));
}

TEST(ServerIPv6SourcePolicy, EachMissingDialPermissionRefusesTheContact)
{
	SServerIPv6SourceFacts notBuilt = ContactableOverIPv6();
	notBuilt.ipv6Built = false;
	ASSERT_FALSE(ShouldContactServerSourceOverIPv6(notBuilt));

	SServerIPv6SourceFacts notPermitted = ContactableOverIPv6();
	notPermitted.familyPermitted = false;
	ASSERT_FALSE(ShouldContactServerSourceOverIPv6(notPermitted));

	SServerIPv6SourceFacts proxied = ContactableOverIPv6();
	proxied.proxyEnabled = true;
	ASSERT_FALSE(ShouldContactServerSourceOverIPv6(proxied));
}
