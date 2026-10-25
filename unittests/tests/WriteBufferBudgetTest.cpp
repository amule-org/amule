// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "WriteBufferBudget.h"

using namespace muleunit;
DECLARE_SIMPLE(WriteBufferBudget)

TEST(WriteBufferBudget, OnlyReceivedFragmentsConsumeCapacity)
{
	WriteBufferBudget budget(20);
	// Selecting arbitrarily many network requests creates no leases.
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
	ASSERT_FALSE(budget.IsPressured());
	auto fragment = budget.Charge(20);
	ASSERT_TRUE(budget.IsPressured());
	// Already requested data remains admissible above the threshold.
	auto inFlight = budget.Charge(10);
	ASSERT_EQUALS(uint64_t{ 30 }, budget.Used());
	fragment.reset();
	ASSERT_EQUALS(uint64_t{ 10 }, budget.Used());
	ASSERT_FALSE(budget.IsPressured());
}

TEST(WriteBufferBudget, FailedFragmentKeepsOnlyItsOwnPayload)
{
	WriteBufferBudget budget(20);
	auto written = budget.Charge(16);
	auto failed = budget.Charge(4);
	ASSERT_TRUE(budget.IsPressured());
	written.reset();
	ASSERT_EQUALS(uint64_t{ 4 }, budget.Used());
	ASSERT_FALSE(budget.IsPressured());
	failed.reset();
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(WriteBufferBudget, PressureHoldsUntilHalfDrained)
{
	WriteBufferBudget budget(20);
	auto first = budget.Charge(11);
	auto second = budget.Charge(9);
	ASSERT_TRUE(budget.IsPressured());
	second.reset();
	ASSERT_TRUE(budget.IsPressured()); // 55% keeps the pressure latch
	first.reset();
	ASSERT_FALSE(budget.IsPressured());
}

TEST(WriteBufferBudget, EarlyFlushStartsAtAnEighth)
{
	WriteBufferBudget budget(80);
	auto first = budget.Charge(9);
	ASSERT_FALSE(budget.ShouldFlush());
	auto second = budget.Charge(1);
	ASSERT_TRUE(budget.ShouldFlush());
	ASSERT_FALSE(budget.IsPressured());
}
