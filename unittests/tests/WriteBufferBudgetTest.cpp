// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "WriteBufferBudget.h"
#include <atomic>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(WriteBufferBudget)

TEST(WriteBufferBudget, GlobalAndAliasedVolumeLimits)
{
	WriteBufferBudget budget(30, 20);
	budget.Identify("a", "disk1");
	budget.Identify("alias", "disk1");
	budget.Identify("b", "disk2");
	auto first = budget.Reserve("a", 15);
	ASSERT_TRUE(first != nullptr);
	ASSERT_TRUE(budget.Reserve("alias", 6) == nullptr);
	auto second = budget.Reserve("alias", 5);
	auto third = budget.Reserve("b", 10);
	ASSERT_TRUE(second != nullptr && third != nullptr);
	ASSERT_TRUE(budget.Reserve("b", 1) == nullptr);
	ASSERT_EQUALS(uint64_t{ 30 }, budget.Used());
	first.reset();
	ASSERT_TRUE(budget.Reserve("b", 10) != nullptr);
}

TEST(WriteBufferBudget, UnknownDebtCannotEscapeThroughDiscovery)
{
	WriteBufferBudget budget(100, 20);
	budget.Identify("known", "disk1");
	auto first = budget.Reserve("known", 12);
	auto unknown = budget.Reserve("unresolved", 8);
	ASSERT_TRUE(first != nullptr && unknown != nullptr);
	ASSERT_TRUE(budget.Reserve("another", 1) == nullptr);
	ASSERT_TRUE(budget.Identify("unresolved", "disk1"));
	ASSERT_TRUE(budget.Reserve("known", 1) == nullptr);
	budget.Identify("independent", "disk2");
	ASSERT_TRUE(budget.Reserve("independent", 20) != nullptr);
	ASSERT_EQUALS(uint64_t{ 20 }, budget.Used());
}

TEST(WriteBufferBudget, UnknownPathsShareOneLimitAndDebtCountsAgainstKnownVolumes)
{
	WriteBufferBudget budget(100, 20);
	auto first = budget.Reserve("one", 13);
	auto second = budget.Reserve("two", 7);
	budget.Identify("known", "disk1");
	ASSERT_TRUE(budget.Reserve("known", 1) == nullptr);
	ASSERT_TRUE(budget.Reserve("three", 1) == nullptr);
	first.reset();
	ASSERT_TRUE(budget.Reserve("known", 13) != nullptr);
}

TEST(WriteBufferBudget, RequestAndFragmentsRetainCreditUntilLastOwner)
{
	WriteBufferBudget budget(20, 20);
	auto request = budget.Reserve("path", 20);
	auto fragment = request;
	auto retry = fragment;
	request.reset();  // disconnect
	fragment.reset(); // one successful write
	ASSERT_TRUE(budget.Reserve("path", 1) == nullptr);
	retry.reset(); // final successful write or cancellation
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
	ASSERT_TRUE(budget.Reserve("path", 20) != nullptr);
}

TEST(WriteBufferBudget, RejectInvalidSizesAndLiveRemount)
{
	WriteBufferBudget budget(100, 20);
	ASSERT_TRUE(budget.Reserve("path", 0) == nullptr);
	ASSERT_TRUE(budget.Reserve("path", std::numeric_limits<uint64_t>::max()) == nullptr);
	budget.Identify("path", "disk1");
	auto lease = budget.Reserve("path", 20);
	ASSERT_FALSE(budget.Identify("path", "disk2"));
	lease.reset();
	ASSERT_TRUE(budget.Identify("path", "disk2"));
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(WriteBufferBudget, ConcurrentAdmissionsAndWorkerReleasesStayBounded)
{
	WriteBufferBudget budget(100, 20);
	std::atomic<bool> exceeded{ false };
	std::vector<std::thread> workers;
	for (unsigned i = 0; i < 8; ++i) {
		workers.emplace_back([&budget, &exceeded] {
			for (unsigned round = 0; round < 1000; ++round) {
				auto lease = budget.Reserve("path", 7);
				if (budget.Used() > 20) {
					exceeded.store(true);
				}
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	ASSERT_FALSE(exceeded.load());
	ASSERT_EQUALS(uint64_t{ 0 }, budget.Used());
}

TEST(WriteBufferBudget, PressureFlushesSmallFilesAndClearsAfterDrain)
{
	WriteBufferBudget budget(100, 20);
	budget.Identify("a", "disk1");
	budget.Identify("b", "disk1");
	auto first = budget.Reserve("a", 7);
	auto second = budget.Reserve("b", 8);
	ASSERT_TRUE(budget.IsPressured("a"));
	first.reset();
	ASSERT_FALSE(budget.IsPressured("a"));
	second.reset();
	ASSERT_FALSE(budget.IsPressured("a"));
}

TEST(WriteBufferBudget, LocalPressureDoesNotFlushIndependentVolumes)
{
	WriteBufferBudget budget(100, 20);
	budget.Identify("hot", "disk1");
	budget.Identify("alias", "disk1");
	budget.Identify("healthy", "disk2");
	auto hot = budget.Reserve("hot", 11);
	auto extra = budget.Reserve("hot", 4);
	auto healthy = budget.Reserve("healthy", 2);
	ASSERT_TRUE(budget.IsPressured("hot"));
	ASSERT_TRUE(budget.IsPressured("alias"));
	ASSERT_FALSE(budget.IsPressured("healthy"));
	extra.reset();
	ASSERT_TRUE(budget.IsPressured("hot")); // 55% retains the pressure latch
	hot.reset();
	ASSERT_FALSE(budget.IsPressured("hot"));
	ASSERT_FALSE(budget.IsPressured("healthy"));
}

TEST(WriteBufferBudget, UnknownPressureCountsOnlyPossibleAliasingDebt)
{
	WriteBufferBudget budget(100, 20);
	budget.Identify("hot", "disk1");
	budget.Identify("healthy", "disk2");
	auto hot = budget.Reserve("hot", 3);
	auto healthy = budget.Reserve("healthy", 2);
	auto unknown = budget.Reserve("unknown", 12);
	ASSERT_TRUE(budget.IsPressured("hot"));
	ASSERT_TRUE(budget.IsPressured("unknown"));
	ASSERT_FALSE(budget.IsPressured("healthy"));
	unknown.reset();
	ASSERT_FALSE(budget.IsPressured("hot"));
	ASSERT_FALSE(budget.IsPressured("unknown"));
}

TEST(WriteBufferBudget, GlobalPressureFlushesAllVolumesWithHysteresis)
{
	WriteBufferBudget budget(100, 40);
	budget.Identify("a", "disk1");
	budget.Identify("b", "disk2");
	budget.Identify("c", "disk3");
	budget.Identify("idle", "disk4");
	auto a = budget.Reserve("a", 25);
	auto b = budget.Reserve("b", 25);
	auto c = budget.Reserve("c", 25);
	ASSERT_TRUE(budget.IsPressured("idle"));
	c.reset(); // 50% clears global pressure
	ASSERT_FALSE(budget.IsPressured("idle"));
	ASSERT_FALSE(budget.IsPressured("a"));
}
