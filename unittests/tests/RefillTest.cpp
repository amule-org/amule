//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
#include "Refill.h"
#include "Types.h"
#include <protocol/ed2k/Constants.h>

#include <deque>

using namespace muleunit;

DECLARE_SIMPLE(Refill)

namespace
{
const uint64_t T0 = 5000000;
}

TEST(Refill, StartsFull)
{
	CRefill budget(5, 20000);
	ASSERT_EQUALS(5u, budget.Available(T0));
}

TEST(Refill, OneUnitBackPerInterval)
{
	CRefill budget(5, 20000);
	budget.Spend(T0, 5);
	ASSERT_EQUALS(0u, budget.Available(T0));
	ASSERT_EQUALS(0u, budget.Available(T0 + 19999));
	ASSERT_EQUALS(1u, budget.Available(T0 + 20000));
	ASSERT_EQUALS(4u, budget.Available(T0 + 80000));
	ASSERT_EQUALS(5u, budget.Available(T0 + 100000));
	ASSERT_EQUALS(5u, budget.Available(T0 + 500000));
}

TEST(Refill, SpendingAgainWaitsForTheDebt)
{
	CRefill budget(5, 20000);
	budget.Spend(T0, 5);
	budget.Spend(T0 + 20000, 1);
	ASSERT_EQUALS(0u, budget.Available(T0 + 20000));
	ASSERT_EQUALS(1u, budget.Available(T0 + 40000));
}

// Asking the connected server whenever the budget allows: Lugdunum answers at most 7 requests
// in any 28 s, and an hour carries the first 5 plus one every 20 s, the old 15 per 300 s.
TEST(Refill, LocalServerBudgetStaysUnderLugdunumsLimit)
{
	CRefill budget(LOCALSERVER_SOURCE_REQUESTS, LOCALSERVER_MS_PER_SOURCE_REQUEST);
	std::deque<uint64_t> window;
	uint32_t maxInWindow = 0;
	uint32_t inFirstHour = 0;
	for (uint64_t now = T0; now < T0 + 2 * 3600 * 1000; now += 100) {
		while (budget.Available(now) > 0) {
			budget.Spend(now, 1);
			window.push_back(now);
			if (now < T0 + 3600 * 1000) {
				++inFirstHour;
			}
		}
		while (!window.empty() && window.front() + 28000 <= now) {
			window.pop_front();
		}
		maxInWindow = std::max<uint32_t>(maxInWindow, window.size());
	}
	ASSERT_TRUE(maxInWindow <= 7);
	ASSERT_EQUALS(184u, inFirstHour);
}
