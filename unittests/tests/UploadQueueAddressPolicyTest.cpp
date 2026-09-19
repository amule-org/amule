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

#include "UploadQueueAddressPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(UploadQueueAddressPolicy)

TEST(UploadQueueAddressPolicy, MatchesNativeIPv6Exactly)
{
	const auto peer = CNetworkAddress::FromString("2001:db8::1");
	ASSERT_TRUE(UploadQueueAddressPolicy::Matches(peer, peer));
	ASSERT_FALSE(UploadQueueAddressPolicy::Matches(peer, CNetworkAddress::FromString("2001:db8::2")));
}

TEST(UploadQueueAddressPolicy, MatchesMappedIPv4AsCanonicalIPv4)
{
	const auto mapped = CNetworkAddress::FromString("::ffff:192.0.2.7");
	const auto plain = CNetworkAddress::FromString("192.0.2.7");
	ASSERT_TRUE(UploadQueueAddressPolicy::Matches(mapped, plain));
}

TEST(UploadQueueAddressPolicy, MatchesRateLimitScopeWithinIpv6Prefix)
{
	ASSERT_TRUE(UploadQueueAddressPolicy::MatchesRateLimitScope(
		CNetworkAddress::FromString("2001:db8:1:2::1"),
		CNetworkAddress::FromString("2001:db8:1:2::ffff")));
	ASSERT_FALSE(UploadQueueAddressPolicy::MatchesRateLimitScope(
		CNetworkAddress::FromString("2001:db8:1:2::1"),
		CNetworkAddress::FromString("2001:db8:1:3::1")));
}

TEST(UploadQueueAddressPolicy, RejectsAbsentAndUnspecifiedAddresses)
{
	const CNetworkAddress absent;
	const auto unspecified = CNetworkAddress::FromString("::");
	const auto peer = CNetworkAddress::FromString("2001:db8::1");

	ASSERT_FALSE(UploadQueueAddressPolicy::Matches(absent, peer));
	ASSERT_FALSE(UploadQueueAddressPolicy::Matches(unspecified, peer));
	ASSERT_FALSE(UploadQueueAddressPolicy::Matches(peer, absent));

	// The cases the guard exists for. Two addresses that name nobody are not the same
	// peer, and without these the assertions above pass on plain inequality alone:
	// removing the guard leaves every one of them true and the test still green.
	ASSERT_FALSE(UploadQueueAddressPolicy::Matches(absent, absent));
	ASSERT_FALSE(UploadQueueAddressPolicy::Matches(unspecified, unspecified));
	ASSERT_FALSE(UploadQueueAddressPolicy::MatchesRateLimitScope(absent, absent));
	ASSERT_FALSE(UploadQueueAddressPolicy::MatchesRateLimitScope(unspecified, unspecified));
	ASSERT_FALSE(UploadQueueAddressPolicy::IsMatchable(absent));
	ASSERT_FALSE(UploadQueueAddressPolicy::IsMatchable(unspecified));
}

// A /64 means "one subscriber" only where one was delegated. Link-local addresses all sit
// under fe80::/64 and NAT64 hosts under one /96, so counting those together would hand a
// whole LAN, or a whole translated site, the budget of a single customer.
TEST(UploadQueueAddressPolicy, RateLimitScopeAggregatesOnlyDelegatedPrefixes)
{
	ASSERT_FALSE(UploadQueueAddressPolicy::MatchesRateLimitScope(
		CNetworkAddress::FromString("fe80::1"), CNetworkAddress::FromString("fe80::2")));
	ASSERT_FALSE(UploadQueueAddressPolicy::MatchesRateLimitScope(
		CNetworkAddress::FromString("64:ff9b::1"), CNetworkAddress::FromString("64:ff9b::2")));
	ASSERT_TRUE(UploadQueueAddressPolicy::MatchesRateLimitScope(
		CNetworkAddress::FromString("fe80::1"), CNetworkAddress::FromString("fe80::1")));

	// Unchanged where it is the point: rotating inside a delegated /64 still counts as one.
	ASSERT_TRUE(UploadQueueAddressPolicy::MatchesRateLimitScope(
		CNetworkAddress::FromString("2001:db8:1:2::1"),
		CNetworkAddress::FromString("2001:db8:1:2::dead:beef")));
}
