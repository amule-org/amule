// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <KadCallbackPolicy.h>

using namespace muleunit;

DECLARE_SIMPLE(KadCallbackPolicy)

TEST(KadCallbackPolicy, ReachabilityTruthTable)
{
	for (bool tcpFirewalled : { false, true }) {
		for (bool udpFirewalled : { false, true }) {
			for (bool udpVerified : { false, true }) {
				const bool direct = Kademlia::DirectCallbackAvailable(
					tcpFirewalled, udpFirewalled, udpVerified);
				const bool buddy =
					Kademlia::NeedsBuddy(tcpFirewalled, udpFirewalled, udpVerified);
				if (!tcpFirewalled) {
					ASSERT_FALSE(direct);
					ASSERT_FALSE(buddy);
				} else if (!udpFirewalled && udpVerified) {
					ASSERT_TRUE(direct);
					ASSERT_FALSE(buddy);
				} else {
					ASSERT_FALSE(direct);
					ASSERT_TRUE(buddy);
				}
			}
		}
	}
}

TEST(KadCallbackPolicy, BuddyToDirectRouteBypassesPublishTimer)
{
	const uint32_t now = 1000;
	const uint32_t lastBuddy = 0x12345678;
	const uint32_t nextPublish = now + 5 * 60 * 60;
	ASSERT_FALSE(Kademlia::CanPublishSource(true, lastBuddy, lastBuddy, nextPublish, now, 4665, 4665));
	// UDP becomes verified open, and ClientList drops the old buddy.
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, lastBuddy, nextPublish, now + 1, 0, 4665));
	// Once the new route is published, the regular throttle applies again.
	ASSERT_FALSE(Kademlia::CanPublishSource(false, 0, 0, nextPublish, now + 2, 0, 0));
}

TEST(KadCallbackPolicy, DirectToBuddyAndChangedBuddyBypassPublishTimer)
{
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 0, 5000, 1000, 4665, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 456, 123, 5000, 1000, 4665, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 456, 456, 5000, 1000, 4665, 4665));
}

TEST(KadCallbackPolicy, NoRouteNeverStartsAPublish)
{
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 123, 5000, 1000, 0, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 123, 0, 1000, 0, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 0, 0, 1000, 0, 0));
}

TEST(KadCallbackPolicy, ChangedBuddyPortBypassesPublishTimer)
{
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 123, 123, 5000, 1000, 4665, 4665));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 123, 5000, 1000, 4675, 4665));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 123, 123, 5000, 1000, 4675, 4675));
	ASSERT_FALSE(Kademlia::CanPublishSource(true, 0, 123, 5000, 1000, 4675, 4665));
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, 123, 5000, 1000, 0, 4665));
}

TEST(KadCallbackPolicy, RepublishBoundaryAndAbortedPublishRetry)
{
	ASSERT_FALSE(Kademlia::CanPublishSource(false, 0, 0, 1000, 999, 0, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, 0, 1000, 1000, 0, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 123, 1000, 1000, 4665, 4665));
	// Search cleared the timestamp after losing its callback route.
	ASSERT_TRUE(Kademlia::CanPublishSource(false, 0, 0, 0, 1000, 0, 0));
	ASSERT_TRUE(Kademlia::CanPublishSource(true, 123, 0, 0, 1000, 4665, 0));
}

namespace
{
constexpr uint32_t MaxWait = 300;

bool Waits(uint32_t nextPublish,
	uint32_t now,
	uint32_t waitingSince,
	bool seedOutOfBudget,
	bool *asked = nullptr)
{
	return Kademlia::KadSourcePublishWaits(nextPublish, now, waitingSince, MaxWait, [&] {
		if (asked) {
			*asked = true;
		}
		return seedOutOfBudget;
	});
}
} // namespace

TEST(KadCallbackPolicy, ADueSourcePublishWaitsOnlyForASeedOutOfBudget)
{
	ASSERT_TRUE(Waits(1000, 1000, 0, true));
	ASSERT_FALSE(Waits(1000, 1000, 0, false));
	// Seeds that have budget again end the wait.
	ASSERT_FALSE(Waits(1000, 1100, 1000, false));
}

// A file due early after a buddy change publishes its new route at once, without asking the seeds.
TEST(KadCallbackPolicy, AFileNotDueNeverWaits)
{
	bool asked = false;
	ASSERT_FALSE(Waits(5000, 1000, 0, true, &asked));
	ASSERT_FALSE(asked);
}

TEST(KadCallbackPolicy, ASourcePublishWaitsAtMostMaxWait)
{
	ASSERT_TRUE(Waits(1000, 1000 + MaxWait - 1, 1000, true));
	bool asked = false;
	ASSERT_FALSE(Waits(1000, 1000 + MaxWait, 1000, true, &asked));
	ASSERT_FALSE(asked);
}
