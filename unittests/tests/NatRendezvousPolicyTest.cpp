//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//

#include <muleunit/test.h>

#include "NatRendezvousPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(NatRendezvousPolicy)

namespace
{
PeerAddressing::UdpEndpoint Endpoint(const char *address, uint16_t port)
{
	return { CNetworkAddress::FromString(address), port };
}
} // namespace

TEST(NatRendezvousPolicy, AcceptsOnlyMatchingObservedEndpoint)
{
	const auto claimed = Endpoint("192.0.2.7", 4672);
	const auto observed = Endpoint("192.0.2.7", 4672);
	PeerAddressing::UdpEndpoint accepted;

	ASSERT_TRUE(NatRendezvous::AcceptObservedEndpoint(claimed, observed, accepted));
	ASSERT_TRUE(observed.address == accepted.address);
	ASSERT_EQUALS(observed.port, accepted.port);
}

TEST(NatRendezvousPolicy, RejectsMismatchedEndpoint)
{
	const auto claimed = Endpoint("192.0.2.7", 4672);
	const auto observed = Endpoint("192.0.2.8", 4672);
	PeerAddressing::UdpEndpoint accepted;

	ASSERT_FALSE(NatRendezvous::AcceptObservedEndpoint(claimed, observed, accepted));
}

TEST(NatRendezvousPolicy, ThrottlesRequesterAndExpiresBudget)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto requester = CNetworkAddress::FromString("192.0.2.7");

	ASSERT_TRUE(limiter.Admit(requester, 1000));
	ASSERT_FALSE(limiter.Admit(requester, 1001));
	ASSERT_TRUE(limiter.Admit(requester, 1000 + NatRendezvous::kRequestThrottleMs));
}

TEST(NatRendezvousPolicy, RejectsUnusableRequester)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_FALSE(limiter.Admit(CNetworkAddress(), 1000));
}

TEST(NatRendezvousPolicy, RejectsClockRollbackAsEarlyRequest)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto requester = CNetworkAddress::FromString("192.0.2.7");

	ASSERT_TRUE(limiter.Admit(requester, 2000));
	ASSERT_FALSE(limiter.Admit(requester, 1000));
}

TEST(NatRendezvousPolicy, SharesBudgetAcrossAnIPv6Slash64)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto first = CNetworkAddress::FromString("2001:db8:1:2::1");
	const auto second = CNetworkAddress::FromString("2001:db8:1:2::2");

	ASSERT_TRUE(limiter.Admit(first, 1000));
	ASSERT_FALSE(limiter.Admit(second, 1001));
}

TEST(NatRendezvousPolicy, KeepsAdjacentIPv6PrefixesIndependent)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto first = CNetworkAddress::FromString("2001:db8:1:2::1");
	const auto second = CNetworkAddress::FromString("2001:db8:1:3::1");

	ASSERT_TRUE(limiter.Admit(first, 1000));
	ASSERT_TRUE(limiter.Admit(second, 1001));
}
