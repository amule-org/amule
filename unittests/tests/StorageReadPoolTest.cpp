// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "StorageReadPool.h"
#include <chrono>
#include <atomic>
#include <future>
#include <stdexcept>

using namespace muleunit;
DECLARE_SIMPLE(StorageReadPool)

namespace
{
struct Job
{
	bool blocked = false;
};
struct Release
{
	std::promise<void> &gate;
	bool opened = false;
	void Open()
	{
		if (!opened) {
			gate.set_value();
			opened = true;
		}
	}
	~Release() { Open(); }
};
} // namespace

TEST(StorageReadPool, ABlockedStorageCannotOccupyOtherWorkers)
{
	std::promise<void> gate;
	auto ready = gate.get_future().share();
	std::promise<void> blocked;
	std::promise<void> healthy;
	StorageReadPool<Job> pool(
		[&](Job &job) {
			if (job.blocked) {
				blocked.set_value();
				ready.wait();
			} else {
				healthy.set_value();
			}
		},
		[] {});
	Release release{ gate };
	auto first = std::make_unique<Job>();
	first->blocked = true;
	ASSERT_TRUE(pool.CanSubmit("slow-device"));
	ASSERT_TRUE(pool.Submit("slow-device", first));
	ASSERT_TRUE(blocked.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	ASSERT_FALSE(pool.CanSubmit("slow-device"));
	ASSERT_TRUE(pool.CanSubmit("healthy-device"));
	for (int i = 0; i < 12; ++i) {
		auto another = std::make_unique<Job>();
		ASSERT_FALSE(pool.Submit("slow-device", another));
		ASSERT_TRUE(another != nullptr);
	}
	auto other = std::make_unique<Job>();
	ASSERT_TRUE(pool.Submit("healthy-device", other));
	ASSERT_TRUE(healthy.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
}

TEST(StorageReadPool, FinishedResultsKeepStorageAndGlobalBudgetsUntilTaken)
{
	std::promise<void> gate;
	auto ready = gate.get_future().share();
	std::promise<void> allFinished;
	std::atomic<unsigned> finished{ 0 };
	StorageReadPool<Job> pool([&](Job &) { ready.wait(); },
		[&] {
			if (++finished == StorageReadPool<Job>::kWorkers) {
				allFinished.set_value();
			}
		});
	Release release{ gate };
	for (size_t i = 0; i < StorageReadPool<Job>::kWorkers; ++i) {
		auto job = std::make_unique<Job>();
		ASSERT_TRUE(pool.Submit(std::to_string(i), job));
	}
	ASSERT_FALSE(pool.HasCapacity());
	ASSERT_FALSE(pool.CanSubmit("other"));
	auto fifth = std::make_unique<Job>();
	ASSERT_FALSE(pool.Submit("other", fifth));
	release.Open();
	ASSERT_TRUE(allFinished.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	ASSERT_FALSE(pool.HasCapacity());
	ASSERT_FALSE(pool.CanSubmit("0"));
	const auto occupied = pool.GetSnapshot();
	ASSERT_EQUALS(StorageReadPool<Job>::kWorkers, occupied.finished);
	ASSERT_EQUALS(size_t{ 0 }, occupied.queued);
	ASSERT_EQUALS(size_t{ 0 }, occupied.running);
	auto duplicate = std::make_unique<Job>();
	ASSERT_FALSE(pool.Submit("0", duplicate));
	size_t completed = 0;
	while (auto result = pool.TakeCompleted().job) {
		++completed;
	}
	ASSERT_EQUALS(StorageReadPool<Job>::kWorkers, completed);
	ASSERT_TRUE(pool.HasCapacity());
	ASSERT_TRUE(pool.CanSubmit("0"));
	ASSERT_TRUE(pool.Submit("0", duplicate));
	pool.Stop();
	ASSERT_TRUE(pool.TakeCompleted().job != nullptr);
	ASSERT_FALSE(pool.CanSubmit("other"));
}

TEST(StorageReadPool, ExceptionsAreReturnedAndDoNotKillWorkers)
{
	std::promise<void> notified;
	StorageReadPool<Job> pool([](Job &) { throw std::runtime_error("controlled read failure"); },
		[&] { notified.set_value(); });
	auto job = std::make_unique<Job>();
	ASSERT_TRUE(pool.Submit("device", job));
	ASSERT_TRUE(notified.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	auto result = pool.TakeCompleted();
	ASSERT_TRUE(result.job != nullptr);
	ASSERT_TRUE(result.error != nullptr);
	ASSERT_TRUE(pool.HasCapacity());
}

TEST(StorageReadPool, DiscoveryReclassifiesAnOutstandingPermit)
{
	std::promise<void> gate;
	auto ready = gate.get_future().share();
	std::promise<void> blocked;
	StorageReadPool<Job> pool(
		[&](Job &) {
			blocked.set_value();
			ready.wait();
		},
		[] {});
	Release release{ gate };
	auto job = std::make_unique<Job>();
	ASSERT_TRUE(pool.Submit("unknown", job));
	ASSERT_TRUE(blocked.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	pool.ReclassifyStorage([](const Job &, const std::string &) { return "device:1"; });
	ASSERT_FALSE(pool.CanSubmit("device:1"));
	ASSERT_TRUE(pool.CanSubmit("device:2"));
	auto alias = std::make_unique<Job>();
	ASSERT_FALSE(pool.Submit("device:1", alias));
	release.Open();
	pool.Stop();
	ASSERT_TRUE(pool.TakeCompleted().job != nullptr);
}
