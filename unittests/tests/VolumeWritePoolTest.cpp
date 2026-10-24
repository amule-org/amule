// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "VolumeWritePool.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <algorithm>

using namespace muleunit;
DECLARE_SIMPLE(VolumeWritePool)

namespace
{
struct Job
{
	int id;
	std::string volume;
};
using Pool = VolumeWritePool<Job>;
struct Control
{
	std::mutex mutex;
	std::condition_variable changed;
	bool release = false, entered = false;
	unsigned probes = 0;
	std::vector<int> done;
	bool Wait(const std::function<bool()> &condition)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(5), condition);
	}
	void Block()
	{
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
	}
	void Release()
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
		changed.notify_all();
	}
	void Done(int id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		done.push_back(id);
		changed.notify_all();
	}
	bool Has(int id) const { return std::find(done.begin(), done.end(), id) != done.end(); }
};
} // namespace

TEST(VolumeWritePool, AliasesCannotConsumeWorkersBehindOneStalledVolume)
{
	Control control;
	Pool pool(
		[&](Job &job) {
			std::lock_guard<std::mutex> lock(control.mutex);
			++control.probes;
			control.changed.notify_all();
			return job.volume;
		},
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				if (job.id == 0) {
					control.Block();
				}
				control.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {});
	ASSERT_TRUE(pool.Submit("first", { 0, "A" }));
	ASSERT_TRUE(control.Wait([&] { return control.entered; }));
	for (int id = 1; id <= 100; ++id) {
		ASSERT_TRUE(pool.Submit(std::to_string(id), { id, "A" }));
	}
	ASSERT_TRUE(pool.Submit("healthy", { 101, "B" }));
	ASSERT_TRUE(control.Wait([&] { return control.Has(101) && control.probes == 102; }));
	{
		std::lock_guard<std::mutex> lock(control.mutex);
		ASSERT_EQUALS(size_t{ 1 }, control.done.size());
	}
	control.Release();
	pool.Stop();
	ASSERT_EQUALS(size_t{ 102 }, control.done.size());
}

TEST(VolumeWritePool, HungDiscoveryDoesNotBlockAnotherVolume)
{
	Control control;
	Pool pool(
		[&](Job &job) {
			if (job.id == 0) {
				control.Block();
			}
			return job.volume;
		},
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				control.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {});
	pool.Submit("hung", { 0, "A" });
	ASSERT_TRUE(control.Wait([&] { return control.entered; }));
	pool.Submit("healthy", { 1, "B" });
	ASSERT_TRUE(control.Wait([&] { return control.Has(1); }));
	control.Release();
	pool.Stop();
	ASSERT_EQUALS(size_t{ 2 }, control.done.size());
}

TEST(VolumeWritePool, UnknownIdentitiesShareOneLane)
{
	Control control;
	Pool pool([](Job &job) { return job.volume; },
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				if (job.id == 0) {
					control.Block();
				}
				control.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {});
	pool.Submit("unknown1", { 0, "" });
	ASSERT_TRUE(control.Wait([&] { return control.entered; }));
	pool.Submit("unknown2", { 1, "" });
	pool.Submit("healthy", { 2, "B" });
	ASSERT_TRUE(control.Wait([&] { return control.Has(2); }));
	{
		std::lock_guard<std::mutex> lock(control.mutex);
		ASSERT_FALSE(control.Has(1));
	}
	control.Release();
	pool.Stop();
	ASSERT_EQUALS(size_t{ 3 }, control.done.size());
}

TEST(VolumeWritePool, QueueBoundAndCancellationPreserveActiveOwnership)
{
	Control control;
	Pool pool([](Job &job) { return job.volume; },
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				control.Block();
				control.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {});
	pool.Submit("same", { 0, "A" });
	ASSERT_TRUE(control.Wait([&] { return control.entered; }));
	for (size_t id = 1; id < Pool::kVolumeJobs; ++id) {
		ASSERT_TRUE(pool.Submit("same", { static_cast<int>(id), "A" }, "A"));
	}
	ASSERT_FALSE(pool.Submit("same", { 9999, "A" }));
	size_t cancelled = 0;
	pool.CancelQueued([](const Job &) { return true; }, [&](Job &) { ++cancelled; });
	ASSERT_EQUALS(Pool::kVolumeJobs - 1, cancelled);
	control.Release();
	pool.Stop();
	ASSERT_EQUALS(size_t{ 1 }, control.done.size());
	ASSERT_FALSE(pool.Submit("after-stop", { 9999, "B" }));
}

TEST(VolumeWritePool, CancelDuringDiscoveryCannotResurrectBehindBusyVolume)
{
	Control active, probe;
	std::atomic<bool> cancelled{ false };
	Pool pool(
		[&](Job &job) {
			if (job.id == 1) {
				probe.Block();
			}
			return job.volume;
		},
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				if (job.id == 0) {
					active.Block();
				}
				active.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {},
		[&](Job &job) {
			if (job.id == 1 && cancelled.load()) {
				active.Done(1);
				return true;
			}
			return false;
		});
	pool.Submit("active", { 0, "A" });
	ASSERT_TRUE(active.Wait([&] { return active.entered; }));
	pool.Submit("probe", { 1, "A" });
	ASSERT_TRUE(probe.Wait([&] { return probe.entered; }));
	cancelled = true;
	pool.CancelQueued([](const Job &job) { return job.id == 1; }, [&](Job &) { active.Done(1); });
	probe.Release();
	ASSERT_TRUE(active.Wait([&] { return active.Has(1); }));
	active.Release();
	pool.Stop();
	ASSERT_EQUALS(size_t{ 2 }, active.done.size());
}

TEST(VolumeWritePool, ExceptionsReleaseVolumeAndStopDrainsAcceptedJobsInOrder)
{
	Control control;
	Pool pool(
		[](Job &job) {
			if (job.id == -2) {
				throw std::runtime_error("discovery");
			}
			return job.volume;
		},
		[&](Pool::Batch &batch) {
			if (batch.front().id == -1) {
				throw std::runtime_error("write");
			}
			for (auto &job : batch) {
				control.Done(job.id);
			}
		},
		[&](Pool::Batch &batch, const std::exception_ptr &) {
			for (auto &job : batch) {
				control.Done(job.id);
			}
		});
	pool.Submit("failed-probe", { -2, "B" });
	pool.Submit("failed-write", { -1, "A" });
	ASSERT_TRUE(control.Wait([&] { return control.Has(-1) && control.Has(-2); }));
	for (int id = 0; id < 200; ++id) {
		pool.Submit("ordered", { id, "A" });
	}
	pool.Stop();
	ASSERT_EQUALS(size_t{ 202 }, control.done.size());
	for (int id = 0; id < 200; ++id) {
		ASSERT_EQUALS(id, control.done[static_cast<size_t>(id) + 2]);
	}
}

TEST(VolumeWritePool, SaturatedVolumeCannotConsumeColdDiscoveryCapacity)
{
	Control control;
	Pool pool([](Job &job) { return job.volume; },
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				if (job.id == 0) {
					control.Block();
				}
				control.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {},
		[&](Job &job) {
			if (job.id == -1) {
				control.Done(-1);
				return true;
			}
			return false;
		});
	pool.Submit("active", { 0, "A" }, "A");
	ASSERT_TRUE(control.Wait([&] { return control.entered; }));
	for (size_t id = 1; id < Pool::kVolumeJobs; ++id) {
		ASSERT_TRUE(pool.Submit("active", { static_cast<int>(id), "A" }, "A"));
	}
	ASSERT_FALSE(pool.Submit("overflow", { 9999, "A" }, "A"));
	ASSERT_TRUE(pool.Submit("cold", { -1, "B" }, "", true));
	ASSERT_TRUE(control.Wait([&] { return control.Has(-1); }));
	ASSERT_TRUE(pool.Submit("cold", { 9999, "B" }, "B"));
	ASSERT_TRUE(control.Wait([&] { return control.Has(9999); }));
	control.Release();
	pool.Stop();
	ASSERT_EQUALS(Pool::kVolumeJobs + 2, control.done.size());
}

TEST(VolumeWritePool, BlockedDiscoveryLeavesWorkersForKnownHealthyVolumes)
{
	Control control;
	Pool pool(
		[&](Job &job) {
			{
				std::lock_guard<std::mutex> lock(control.mutex);
				++control.probes;
				control.changed.notify_all();
			}
			control.Block();
			return job.volume;
		},
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				control.Done(job.id);
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {});
	for (int id = 0; id < 3; ++id) {
		pool.Submit(std::to_string(id), { id, "A" });
	}
	ASSERT_TRUE(control.Wait([&] { return control.probes == Pool::kResolvers; }));
	ASSERT_TRUE(pool.Submit("known", { 3, "B" }, "B"));
	ASSERT_TRUE(control.Wait([&] { return control.Has(3); }));
	control.Release();
	pool.Stop();
	ASSERT_EQUALS(size_t{ 4 }, control.done.size());
}

TEST(VolumeWritePool, ContinuousResolvedWritesCannotStarveColdDiscovery)
{
	std::mutex mutex;
	std::condition_variable changed;
	unsigned initial = 0, processed = 0, parked = 0;
	bool begin = false, finish = false, discovered = false, submissionFailed = false;
	Pool *owner = nullptr;
	Pool pool(
		[&](Job &) {
			std::lock_guard<std::mutex> lock(mutex);
			discovered = true;
			changed.notify_all();
			return std::string("new-volume");
		},
		[&](Pool::Batch &batch) {
			for (auto &job : batch) {
				{
					std::unique_lock<std::mutex> lock(mutex);
					if (!begin) {
						++initial;
						changed.notify_all();
						changed.wait_for(
							lock, std::chrono::seconds(5), [&] { return begin; });
					}
					if (finish || discovered) {
						continue;
					}
				}
				const bool accepted = owner->Submit(job.volume, job, job.volume);
				std::unique_lock<std::mutex> lock(mutex);
				// Stop may reject a replenishment already racing with test teardown.
				submissionFailed |= !accepted && !finish;
				++processed;
				if (processed >= 100) {
					++parked;
					changed.notify_all();
					changed.wait_for(
						lock, std::chrono::seconds(5), [&] { return finish; });
				}
			}
		},
		[](Pool::Batch &, const std::exception_ptr &) {},
		[](Job &job) { return job.id == -1; });
	owner = &pool;
	for (const auto *key : { "A", "B", "C", "D" }) {
		pool.Submit(key, { 0, key }, key);
	}
	bool allWorkersStarted;
	{
		std::unique_lock<std::mutex> lock(mutex);
		allWorkersStarted =
			changed.wait_for(lock, std::chrono::seconds(5), [&] { return initial == 4; });
	}
	const bool admitted = pool.Submit("cold", { -1, "new-volume" }, "", true);
	bool discoveredDuringLoad;
	{
		std::unique_lock<std::mutex> lock(mutex);
		begin = true;
		changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(5), [&] { return discovered || parked == 4; });
		discoveredDuringLoad = discovered;
		finish = true;
		changed.notify_all();
	}
	pool.Stop();
	ASSERT_TRUE(allWorkersStarted);
	ASSERT_TRUE(admitted);
	ASSERT_FALSE(submissionFailed);
	ASSERT_TRUE(discoveredDuringLoad);
	ASSERT_TRUE(discovered);
}
