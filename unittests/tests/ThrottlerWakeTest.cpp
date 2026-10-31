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

#include <muleunit/test.h>
#include "ThrottlerWake.h"

#include <chrono>
#include <thread>

using namespace muleunit;
DECLARE_SIMPLE(ThrottlerWake)

namespace
{
// Far longer than any test may take, so a missed wake fails the time check.
const uint64 LONG_WAIT_MS = 10000;

uint64 WaitMs(CThrottlerWake &wake, uint64 timeoutMs, bool &woken)
{
	const auto start = std::chrono::steady_clock::now();
	woken = wake.Wait(timeoutMs);
	return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start)
					   .count());
}
} // namespace

TEST(ThrottlerWake, WakeBeforeWaitIsKept)
{
	CThrottlerWake wake;
	wake.Notify();

	bool woken = false;
	ASSERT_TRUE(WaitMs(wake, LONG_WAIT_MS, woken) < 1000);
	ASSERT_TRUE(woken);
}

TEST(ThrottlerWake, WaitConsumesTheWake)
{
	CThrottlerWake wake;
	wake.Notify();
	ASSERT_TRUE(wake.Wait(0));
	ASSERT_FALSE(wake.Wait(20));
}

TEST(ThrottlerWake, WakesCoalesce)
{
	CThrottlerWake wake;
	wake.Notify();
	wake.Notify();
	wake.Notify();

	ASSERT_TRUE(wake.Wait(0));
	ASSERT_FALSE(wake.Wait(0));
}

TEST(ThrottlerWake, ZeroTimeoutDoesNotSleep)
{
	CThrottlerWake wake;

	bool woken = true;
	ASSERT_TRUE(WaitMs(wake, 0, woken) < 1000);
	ASSERT_FALSE(woken);
}

TEST(ThrottlerWake, WakeFromAnotherThreadEndsTheWait)
{
	CThrottlerWake wake;
	std::thread notifier([&wake]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		wake.Notify();
	});

	// A spurious wakeup returns false; wait again, as the throttler loop would.
	bool woken = false;
	uint64 elapsed = 0;
	while (!woken && elapsed < LONG_WAIT_MS) {
		bool thisWake = false;
		elapsed += WaitMs(wake, LONG_WAIT_MS, thisWake);
		woken = thisWake;
	}
	notifier.join();
	ASSERT_TRUE(woken);
	ASSERT_TRUE(elapsed < LONG_WAIT_MS);
}
