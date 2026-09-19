//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
}
