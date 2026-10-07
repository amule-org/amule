// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <kademlia/kademlia/FirewallRecheck.h>

using namespace muleunit;
using Kademlia::CFirewallRecheck;

DECLARE_SIMPLE(KadFirewallRecheck)

namespace
{
const time_t kStart = 1000000;

// An open node after a finished recheck: the state the hourly recheck starts from.
CFirewallRecheck OpenNode()
{
	CFirewallRecheck fw;
	for (uint32_t i = 0; i < KADEMLIAFIREWALLCHECKS; ++i) {
		fw.AddAnswer(kStart - 100);
	}
	fw.AddConfirmation();
	fw.AddConfirmation();
	return fw;
}

void AddAllAnswers(CFirewallRecheck &fw, time_t now)
{
	for (uint32_t i = 0; i < KADEMLIAFIREWALLCHECKS; ++i) {
		fw.AddAnswer(now);
	}
}
} // namespace

TEST(KadFirewallRecheck, StartsFirewalledUntilConfirmed)
{
	CFirewallRecheck fw;
	ASSERT_TRUE(fw.IsFirewalled(kStart));
	AddAllAnswers(fw, kStart);
	ASSERT_TRUE(fw.IsFirewalled(kStart + 1));
	fw.AddConfirmation();
	fw.AddConfirmation();
	ASSERT_FALSE(fw.IsFirewalled(kStart + 2));
}

// The reported race: every IP answer arrives before the TCP confirmations. The node must not
// show as firewalled in between.
TEST(KadFirewallRecheck, AnswersBeforeConfirmationsKeepOpenState)
{
	CFirewallRecheck fw = OpenNode();
	fw.Start(kStart);
	ASSERT_FALSE(fw.IsFirewalled(kStart));
	AddAllAnswers(fw, kStart + 1);
	ASSERT_FALSE(fw.AwaitingAnswers());
	ASSERT_FALSE(fw.IsFirewalled(kStart + 1));
	fw.AddConfirmation();
	ASSERT_FALSE(fw.IsFirewalled(kStart + 2));
	fw.AddConfirmation();
	ASSERT_FALSE(fw.IsFirewalled(kStart + 2));
}

TEST(KadFirewallRecheck, ConfirmationsBeforeAnswersKeepOpenState)
{
	CFirewallRecheck fw = OpenNode();
	fw.Start(kStart);
	fw.AddConfirmation();
	fw.AddConfirmation();
	ASSERT_FALSE(fw.IsFirewalled(kStart + 1));
	AddAllAnswers(fw, kStart + 2);
	ASSERT_FALSE(fw.IsFirewalled(kStart + 2));
}

// A node that really became unreachable still turns firewalled, once the grace period is over.
TEST(KadFirewallRecheck, NoConfirmationTurnsFirewalledAfterGrace)
{
	CFirewallRecheck fw = OpenNode();
	fw.Start(kStart);
	AddAllAnswers(fw, kStart + 1);
	ASSERT_FALSE(fw.IsFirewalled(kStart + CFirewallRecheck::kGraceSeconds));
	ASSERT_TRUE(fw.IsFirewalled(kStart + 1 + CFirewallRecheck::kGraceSeconds));
	// One confirmation is not enough.
	fw.AddConfirmation();
	ASSERT_TRUE(fw.IsFirewalled(kStart + 2 + CFirewallRecheck::kGraceSeconds));
}

// A recheck started while one is still running keeps the state that was showing.
TEST(KadFirewallRecheck, RestartDuringRecheckKeepsShownState)
{
	CFirewallRecheck fw = OpenNode();
	fw.Start(kStart);
	fw.Start(kStart + 5);
	ASSERT_FALSE(fw.IsFirewalled(kStart + 5));
}

// More answers than requested must not restart the grace period.
TEST(KadFirewallRecheck, LateAnswersDoNotExtendGrace)
{
	CFirewallRecheck fw = OpenNode();
	fw.Start(kStart);
	AddAllAnswers(fw, kStart);
	fw.AddAnswer(kStart + 50);
	ASSERT_TRUE(fw.IsFirewalled(kStart + CFirewallRecheck::kGraceSeconds));
}
