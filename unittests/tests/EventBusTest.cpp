//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include <muleunit/test.h>

#include "EventBus.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(EventBus)

TEST(EventBus, EmptyBusOldestAndNewestAreZero)
{
	CEventBus bus;
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), bus.OldestId());
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), bus.NewestId());
}

TEST(EventBus, PublishAssignsMonotonicIds)
{
	CEventBus bus;
	bus.Publish("a", "{}");
	bus.Publish("b", "{}");
	bus.Publish("c", "{}");
	// First id is 1; ids are dense and monotonic.
	ASSERT_EQUALS(static_cast<std::uint64_t>(1), bus.OldestId());
	ASSERT_EQUALS(static_cast<std::uint64_t>(3), bus.NewestId());
}

TEST(EventBus, DrainSinceZeroReturnsEverything)
{
	CEventBus bus;
	bus.Publish("a", "{\"x\":1}");
	bus.Publish("b", "{\"x\":2}");

	std::vector<Event> out;
	const std::uint64_t high = bus.Drain(0, std::chrono::milliseconds(0), out);
	ASSERT_EQUALS(static_cast<size_t>(2), out.size());
	ASSERT_EQUALS(std::string("a"), out[0].name);
	ASSERT_EQUALS(std::string("b"), out[1].name);
	ASSERT_EQUALS(static_cast<std::uint64_t>(2), high);
}

TEST(EventBus, DrainSinceFiltersOlder)
{
	CEventBus bus;
	bus.Publish("a", "{}");
	bus.Publish("b", "{}");
	bus.Publish("c", "{}");
	std::vector<Event> out;
	const std::uint64_t high = bus.Drain(/*since=*/1, std::chrono::milliseconds(0), out);
	ASSERT_EQUALS(static_cast<size_t>(2), out.size());
	ASSERT_EQUALS(std::string("b"), out[0].name);
	ASSERT_EQUALS(std::string("c"), out[1].name);
	ASSERT_EQUALS(static_cast<std::uint64_t>(3), high);
}

TEST(EventBus, DrainBlocksUntilPublish)
{
	CEventBus bus;
	std::atomic<bool> drain_returned{ false };
	std::vector<Event> got;

	std::thread waiter([&] {
		bus.Drain(0, std::chrono::seconds(5), got);
		drain_returned.store(true);
	});

	// Drainer should still be blocked after a beat.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	ASSERT_FALSE(drain_returned.load());

	// Publish — the condvar should wake the drainer in well under 5 s.
	bus.Publish("late", "{}");

	// Give a generous slack window for the wake + copy.
	for (int i = 0; i < 50 && !drain_returned.load(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	ASSERT_TRUE(drain_returned.load());
	waiter.join();

	ASSERT_EQUALS(static_cast<size_t>(1), got.size());
	ASSERT_EQUALS(std::string("late"), got[0].name);
}

TEST(EventBus, DrainTimesOutWhenNothingPublished)
{
	CEventBus bus;
	std::vector<Event> out;
	const auto start = std::chrono::steady_clock::now();
	const std::uint64_t high = bus.Drain(0, std::chrono::milliseconds(120), out);
	const auto elapsed = std::chrono::steady_clock::now() - start;

	ASSERT_TRUE(out.empty());
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), high);
	// The drain should have spent the full timeout waiting; 80 ms of
	// slack to absorb scheduling jitter.
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	ASSERT_TRUE(ms >= 80);
}

TEST(EventBus, RingCapDropsOldestWhenFull)
{
	// Construct with an explicit small capacity rather than the
	// default — the default is sized for real workloads (16K) and
	// this test would otherwise publish 16K+ events to exercise it.
	CEventBus bus(/*capacity=*/64);
	const std::size_t over = bus.Capacity() + 5;
	for (std::size_t i = 0; i < over; ++i) {
		bus.Publish("x", "{}");
	}

	std::vector<Event> out;
	bus.Drain(0, std::chrono::milliseconds(0), out);
	ASSERT_EQUALS(bus.Capacity(), out.size());

	// OldestId is the first id we STILL have (= ids dropped + 1).
	const std::uint64_t expected_oldest = (over - bus.Capacity()) + 1;
	ASSERT_EQUALS(expected_oldest, bus.OldestId());
	ASSERT_EQUALS(static_cast<std::uint64_t>(over), bus.NewestId());
}

TEST(EventBus, ExplicitCapacityHonored)
{
	CEventBus bus(/*capacity=*/256);
	ASSERT_EQUALS(static_cast<std::size_t>(256), bus.Capacity());
}

TEST(EventBus, BelowMinCapacityIsClampedUp)
{
	// Operator config below the floor (e.g. capacity=1) is clamped
	// to kMinCapacity. Without the clamp the bus would effectively
	// disable replay; the floor guarantees a meaningful window.
	CEventBus bus(/*capacity=*/1);
	ASSERT_EQUALS(CEventBus::kMinCapacity, bus.Capacity());
}

TEST(EventBus, ConcurrentPublishersHaveDistinctIds)
{
	CEventBus bus;
	const int per_thread = 50;
	const int n_threads = 4;
	std::vector<std::thread> ths;
	for (int t = 0; t < n_threads; ++t) {
		ths.emplace_back([&] {
			for (int i = 0; i < per_thread; ++i) {
				bus.Publish("p", "{}");
			}
		});
	}
	for (auto &t : ths)
		t.join();

	const int total = per_thread * n_threads;
	// We can have dropped some if total > capacity; what's *guaranteed*
	// is the newest id == total (every Publish atomically grabbed a
	// unique id).
	ASSERT_EQUALS(static_cast<std::uint64_t>(total), bus.NewestId());

	std::vector<Event> out;
	bus.Drain(0, std::chrono::milliseconds(0), out);
	// All retained ids are unique and dense in their tail of the
	// monotonic sequence.
	std::uint64_t prev = 0;
	for (const auto &ev : out) {
		ASSERT_TRUE(ev.id > prev);
		prev = ev.id;
	}
}
