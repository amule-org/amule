// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "UploadPipelinePressure.h"
#include <limits>

using namespace muleunit;
DECLARE_SIMPLE(UploadPipelinePressure)
using Monitor = UploadPipelinePressure;

TEST(UploadPipelinePressure, ControlledReadWaitPersistsAndRecoveryClearsIt)
{
	Monitor monitor;
	Monitor::Input input;
	input.wireValid = true;
	input.limit = 100000;
	input.AddClient(true, true, false, {});
	input.storageDenied = true;
	for (uint64_t now = 0; now <= 10000; now += 500) {
		const auto result = monitor.Observe(input, now);
		ASSERT_TRUE(result.signals[Monitor::ReadWait]);
		ASSERT_FALSE(result.signals[Monitor::TransportBusy]);
		ASSERT_TRUE(result.sustainedUnderfill == (now >= 2000));
		ASSERT_TRUE(result.persistentUnderfill == (now >= 10000));
	}
	input.wireRate = input.limit;
	const auto recovered = monitor.Observe(input, 10500);
	ASSERT_FALSE(recovered.sustainedUnderfill);
	ASSERT_FALSE(recovered.persistentUnderfill);
	ASSERT_EQUALS(uint64_t{ 0 }, recovered.ageMs[Monitor::Underfilled]);
	ASSERT_TRUE(recovered.signals[Monitor::ReadWait]);
}

TEST(UploadPipelinePressure, ControlledReceiverBackpressureIsDifferentFromReadWait)
{
	Monitor monitor;
	Monitor::Input input;
	input.wireValid = true;
	input.limit = 100000;
	input.AddClient(true, false, false, { 184320, true });
	auto result = monitor.Observe(input, 0);
	ASSERT_TRUE(result.signals[Monitor::TransportBusy]);
	ASSERT_FALSE(result.signals[Monitor::ReadWait]);
	ASSERT_FALSE(result.signals[Monitor::AdmissionWait]);
	ASSERT_EQUALS(uint64_t{ 184320 }, result.input.readyBytes);
	ASSERT_EQUALS(uint64_t{ 0 }, result.input.ready);
	// A transport can still own an asynchronous write after payload leaves the queue.
	Monitor::Input transportOnly;
	transportOnly.wireValid = true;
	transportOnly.limit = input.limit;
	transportOnly.AddClient(false, false, false, { 0, true });
	result = monitor.Observe(transportOnly, 500);
	ASSERT_TRUE(result.signals[Monitor::Underfilled]);
	ASSERT_TRUE(result.signals[Monitor::TransportBusy]);
}

TEST(UploadPipelinePressure, ControlledMemoryAndWorkerLimitsKeepSeparateReasons)
{
	Monitor monitor;
	Monitor::Input input;
	input.wireValid = true;
	input.limit = 100000;
	input.AddClient(true, false, false, {});
	input.clientMemoryDenied = true;
	auto result = monitor.Observe(input, 0);
	ASSERT_TRUE(result.signals[Monitor::AdmissionWait]);
	ASSERT_TRUE(result.signals[Monitor::ClientMemory]);
	ASSERT_FALSE(result.signals[Monitor::GlobalMemory]);
	ASSERT_FALSE(result.signals[Monitor::ReadWait]);
	input.clientMemoryDenied = false;
	input.globalMemoryDenied = true;
	input.workersFull = true;
	result = monitor.Observe(input, 500);
	ASSERT_FALSE(result.signals[Monitor::ClientMemory]);
	ASSERT_TRUE(result.signals[Monitor::GlobalMemory]);
	ASSERT_TRUE(result.signals[Monitor::WorkersFull]);
}

TEST(UploadPipelinePressure, MixedClientsAndFileCompletionAreNotCollapsedIntoOneCause)
{
	Monitor monitor;
	Monitor::Input input;
	input.AddClient(true, true, false, {});
	input.AddClient(true, false, false, { 100, true });
	input.AddClient(true, false, false, { 200, false });
	input.AddClient(true, false, true, {});
	input.AddClient(false, false, false, {});
	const auto result = monitor.Observe(input, 100);
	ASSERT_EQUALS(uint64_t{ 5 }, result.input.active);
	ASSERT_EQUALS(uint64_t{ 1 }, result.input.drained);
	ASSERT_EQUALS(uint64_t{ 1 }, result.input.ready);
	ASSERT_EQUALS(uint64_t{ 300 }, result.input.readyBytes);
	ASSERT_TRUE(result.signals[Monitor::ReadWait]);
	ASSERT_TRUE(result.signals[Monitor::TransportBusy]);
	ASSERT_TRUE(result.signals[Monitor::Deferred]);
	ASSERT_FALSE(result.signals[Monitor::AdmissionWait]);
}

TEST(UploadPipelinePressure, IdleUnlimitedAndConfiguredCapDoNotImplyUnderfill)
{
	Monitor monitor;
	Monitor::Input idle;
	idle.wireValid = true;
	idle.limit = 100000;
	idle.AddClient(false, false, false, {});
	ASSERT_FALSE(monitor.Observe(idle, 0).signals[Monitor::Underfilled]);
	Monitor::Input input;
	input.wireValid = true;
	input.AddClient(true, false, false, { 1, false });
	ASSERT_FALSE(monitor.Observe(input, 500).signals[Monitor::Underfilled]);
	input.wireValid = true;
	input.limit = 100000;
	input.wireRate = 98000;
	ASSERT_FALSE(monitor.Observe(input, 1000).signals[Monitor::Underfilled]);
	input.wireRate = 97999;
	ASSERT_TRUE(monitor.Observe(input, 1500).signals[Monitor::Underfilled]);
	input.wireValid = true;
	input.limit = 1024;
	input.wireRate = 0;
	ASSERT_TRUE(monitor.Observe(input, 2000).signals[Monitor::Underfilled]);
	input.wireValid = true;
	input.limit = std::numeric_limits<uint64_t>::max();
	input.wireRate = input.limit;
	ASSERT_FALSE(monitor.Observe(input, 2500).signals[Monitor::Underfilled]);
}

TEST(UploadPipelinePressure, MissingObservationsAndClockResetBreakPersistence)
{
	Monitor monitor;
	Monitor::Input input;
	input.wireValid = true;
	input.limit = 100000;
	input.AddClient(true, true, false, {});
	monitor.Observe(input, 0);
	monitor.Observe(input, 1000);
	ASSERT_TRUE(monitor.Observe(input, 2000).sustainedUnderfill);
	const auto afterGap = monitor.Observe(input, 4000);
	ASSERT_FALSE(afterGap.sustainedUnderfill);
	ASSERT_EQUALS(uint64_t{ 0 }, afterGap.ageMs[Monitor::ReadWait]);
	ASSERT_FALSE(monitor.Observe(input, 1).sustainedUnderfill);
}

TEST(UploadPipelinePressure, ControlledCompletionBacklogRemainsObservable)
{
	Monitor monitor;
	Monitor::Input input;
	input.AddClient(true, true, false, {});
	input.finishedJobs = 4;
	monitor.Observe(input, 0);
	monitor.Observe(input, 1000);
	const auto held = monitor.Observe(input, 2000);
	ASSERT_TRUE(held.signals[Monitor::CompletionsReady]);
	ASSERT_EQUALS(uint64_t{ 2000 }, held.ageMs[Monitor::CompletionsReady]);
	ASSERT_EQUALS(uint64_t{ 0 }, held.input.runningJobs);
	input.finishedJobs = 0;
	ASSERT_FALSE(monitor.Observe(input, 2500).signals[Monitor::CompletionsReady]);
}

TEST(UploadPipelinePressure, WireRateExpiresWithoutDisplayOrSessionTimerUpdates)
{
	Monitor monitor;
	ASSERT_FALSE(monitor.ObserveTraffic(1000, 0).valid);
	ASSERT_FALSE(monitor.ObserveTraffic(3000, 500).valid);
	auto rate = monitor.ObserveTraffic(5000, 1000);
	ASSERT_TRUE(rate.valid);
	ASSERT_EQUALS(uint64_t{ 4000 }, rate.rate);
	ASSERT_EQUALS(uint64_t{ 1000 }, rate.windowMs);
	rate = monitor.ObserveTraffic(5000, 1500);
	ASSERT_EQUALS(uint64_t{ 2000 }, rate.rate);
	rate = monitor.ObserveTraffic(5000, 2000);
	ASSERT_TRUE(rate.valid);
	ASSERT_EQUALS(uint64_t{ 0 }, rate.rate);
	ASSERT_FALSE(monitor.ObserveTraffic(9000, 4000).valid);
	ASSERT_FALSE(monitor.ObserveTraffic(0, 4500).valid); // replaced/reset producer
	ASSERT_FALSE(monitor.ObserveTraffic(1, 4500).valid); // same timestamp
}

TEST(UploadPipelinePressure, UnderfillWaitsForValidTrafficAndRateArithmeticIsBounded)
{
	Monitor monitor;
	Monitor::Input input;
	input.limit = 100000;
	input.AddClient(true, true, false, {});
	ASSERT_FALSE(monitor.Observe(input, 0).signals[Monitor::Underfilled]);
	ASSERT_TRUE(monitor.Observe(input, 0).signals[Monitor::ReadWait]);
	monitor.ObserveTraffic(0, 0);
	const auto rate = monitor.ObserveTraffic(std::numeric_limits<uint64_t>::max(), 1);
	ASSERT_EQUALS(std::numeric_limits<uint64_t>::max(), rate.rate);
	ASSERT_FALSE(rate.valid);
}
