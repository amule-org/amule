// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "WriteBufferBudget.h"

using namespace muleunit;
DECLARE_SIMPLE(WriteBufferBudget)

TEST(WriteBufferBudget, OnlyReceivedFragmentsConsumeCapacity)
{
	WriteBufferBudget budget;
	const uint64_t limit = 20;
	// Selecting arbitrarily many network requests creates no leases.
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
	ASSERT_FALSE(budget.IsPressured(limit));
	auto fragment = budget.Charge(20);
	ASSERT_TRUE(budget.IsPressured(limit));
	// Already requested data remains admissible above the threshold.
	auto inFlight = budget.Charge(10);
	ASSERT_EQUALS(uint64_t{ 30 }, budget.Used());
	fragment.reset();
	ASSERT_EQUALS(uint64_t{ 10 }, budget.Used());
	ASSERT_FALSE(budget.IsPressured(limit));
}

TEST(WriteBufferBudget, FailedFragmentKeepsOnlyItsOwnPayload)
{
	WriteBufferBudget budget;
	const uint64_t limit = 20;
	auto written = budget.Charge(16);
	auto failed = budget.Charge(4);
	ASSERT_TRUE(budget.IsPressured(limit));
	written.reset();
	ASSERT_EQUALS(uint64_t{ 4 }, budget.Used());
	ASSERT_FALSE(budget.IsPressured(limit));
	failed.reset();
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(WriteBufferBudget, PressureHoldsUntilHalfDrained)
{
	WriteBufferBudget budget;
	const uint64_t limit = 20;
	auto first = budget.Charge(11);
	auto second = budget.Charge(9);
	ASSERT_TRUE(budget.IsPressured(limit));
	second.reset();
	ASSERT_TRUE(budget.IsPressured(limit)); // 55% keeps the pressure latch
	first.reset();
	ASSERT_FALSE(budget.IsPressured(limit));
}

TEST(WriteBufferBudget, EarlyFlushStartsAtAnEighth)
{
	WriteBufferBudget budget;
	const uint64_t limit = 80;
	auto first = budget.Charge(9);
	ASSERT_FALSE(budget.ShouldFlush(limit));
	auto second = budget.Charge(1);
	ASSERT_TRUE(budget.ShouldFlush(limit));
	ASSERT_FALSE(budget.IsPressured(limit));
}

TEST(WriteBufferBudget, LiveLimitChangesKeepCreditsAndReevaluatePressure)
{
	WriteBufferBudget budget;
	auto first = budget.Charge(12);
	auto second = budget.Charge(8);
	ASSERT_TRUE(budget.IsPressured(20));
	// Raising the limit enough resumes selection without releasing payload.
	ASSERT_FALSE(budget.IsPressured(40));
	ASSERT_EQUALS(uint64_t{ 20 }, budget.Used());
	// Lowering it stops selection immediately, including below the current use.
	ASSERT_TRUE(budget.IsPressured(16));
	second.reset();
	ASSERT_TRUE(budget.IsPressured(16));   // 75% retains the pressure latch
	ASSERT_FALSE(budget.ShouldFlush(104)); // 12 bytes is below one eighth
	ASSERT_TRUE(budget.ShouldFlush(96));
	first.reset();
	ASSERT_FALSE(budget.IsPressured(16));
}
