//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//

#include <muleunit/test.h>

#include "NatRendezvousPolicy.h"

#include <string>

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
	const auto accepted = NatRendezvous::AcceptObservedEndpoint(claimed, observed);

	ASSERT_TRUE(accepted.has_value());
	ASSERT_TRUE(observed.address == accepted->address);
	ASSERT_EQUALS(observed.port, accepted->port);
}

TEST(NatRendezvousPolicy, RejectsMismatchedEndpoint)
{
	const auto claimed = Endpoint("192.0.2.7", 4672);
	const auto observed = Endpoint("192.0.2.8", 4672);
	ASSERT_FALSE(NatRendezvous::AcceptObservedEndpoint(claimed, observed).has_value());
	ASSERT_FALSE(NatRendezvous::AcceptObservedEndpoint(claimed, Endpoint("192.0.2.7", 4673)).has_value());
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
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("0.0.0.0"), 1000));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("::"), 1000));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("::ffff:0.0.0.0"), 1000));
}

TEST(NatRendezvousPolicy, ReanchorsWindowOnClockRollback)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto requester = CNetworkAddress::FromString("192.0.2.7");

	ASSERT_TRUE(limiter.Admit(requester, 2000));
	ASSERT_TRUE(limiter.Admit(requester, 1000));
	ASSERT_FALSE(limiter.Admit(requester, 1001));
	ASSERT_FALSE(limiter.Admit(requester, 1000 + NatRendezvous::kRequestThrottleMs - 1));
	ASSERT_TRUE(limiter.Admit(requester, 1000 + NatRendezvous::kRequestThrottleMs));
}

TEST(NatRendezvousPolicy, EvictsOldestScopeToAdmit257thRequester)
{
	NatRendezvous::CRequesterLimiter limiter;
	// Insert the oldest entry last so eviction cannot rely on insertion order.
	for (std::size_t i = 1; i < NatRendezvous::kMaxTrackedRequesters; ++i) {
		const auto address = "192.0.2." + std::to_string(i);
		ASSERT_TRUE(limiter.Admit(CNetworkAddress::FromString(address.c_str()), 1001));
	}
	const auto oldest = CNetworkAddress::FromString("192.0.2.0");
	const auto newest = CNetworkAddress::FromString("192.0.3.1");
	ASSERT_TRUE(limiter.Admit(oldest, 1000));
	ASSERT_TRUE(limiter.Admit(newest, 1002));
	ASSERT_FALSE(limiter.Admit(newest, 1003));
	ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString("192.0.2.1"), 1003));
	ASSERT_TRUE(limiter.Admit(oldest, 1003));
}

TEST(NatRendezvousPolicy, FreesExpiredSlotBeforeEvictingLiveScope)
{
	NatRendezvous::CRequesterLimiter limiter;
	for (std::size_t i = 0; i < NatRendezvous::kMaxTrackedRequesters; ++i) {
		const auto address = "192.0.2." + std::to_string(i);
		ASSERT_TRUE(
			limiter.Admit(CNetworkAddress::FromString(address.c_str()), i == 0 ? 1000 : 1001));
	}
	const auto now = 1000 + NatRendezvous::kRequestThrottleMs;
	ASSERT_TRUE(limiter.Admit(CNetworkAddress::FromString("192.0.3.1"), now));
	for (std::size_t i = 1; i < NatRendezvous::kMaxTrackedRequesters; ++i) {
		const auto address = "192.0.2." + std::to_string(i);
		ASSERT_FALSE(limiter.Admit(CNetworkAddress::FromString(address.c_str()), now));
	}
}

TEST(NatRendezvousPolicy, SharesBudgetAcrossMappedAndPlainIPv4)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto plain = CNetworkAddress::FromString("192.0.2.7");
	const auto mapped = CNetworkAddress::FromString("::ffff:192.0.2.7");

	ASSERT_TRUE(limiter.Admit(plain, 1000));
	ASSERT_FALSE(limiter.Admit(mapped, 1001));
	ASSERT_TRUE(limiter.Admit(mapped, 1000 + NatRendezvous::kRequestThrottleMs));
	ASSERT_FALSE(limiter.Admit(plain, 1001 + NatRendezvous::kRequestThrottleMs));
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
