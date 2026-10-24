// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "UploadReadDiagnostics.h"
#include <stdexcept>

using namespace muleunit;
DECLARE_SIMPLE(UploadReadDiagnostics)

TEST(UploadReadDiagnostics, SeparatesQueueWorkAndCompletionDelay)
{
	UploadReadDiagnostics diagnostics;
	uint64_t clock = 17;
	UploadReadTiming timing;
	timing.submitted = 10;
	timing.Run([&clock] { clock += 1500; }, [&clock] { return clock; });
	diagnostics.Completed(timing, 1540, false, false, false);
	const auto stats = diagnostics.Take(1600);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.work.count);
	ASSERT_EQUALS(uint64_t{ 7 }, stats.queue.totalMs);
	ASSERT_EQUALS(uint64_t{ 1500 }, stats.work.totalMs);
	ASSERT_EQUALS(uint64_t{ 1500 }, stats.work.maxMs);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.work.overSecond);
	ASSERT_EQUALS(uint64_t{ 23 }, stats.completion.totalMs);
	ASSERT_EQUALS(uint64_t{ 0 }, diagnostics.Take(1700).work.count);
}

TEST(UploadReadDiagnostics, FailedWorkStillPublishesItsFinishTime)
{
	UploadReadTiming timing;
	uint64_t clock = 100;
	bool caught = false;
	try {
		timing.Run(
			[&clock] {
				clock = 400;
				throw std::runtime_error("controlled I/O failure");
			},
			[&clock] { return clock; });
	} catch (const std::runtime_error &) {
		caught = true;
	}
	ASSERT_TRUE(caught);
	ASSERT_EQUALS(uint64_t{ 100 }, timing.started);
	ASSERT_EQUALS(uint64_t{ 400 }, timing.finished);
	UploadReadDiagnostics diagnostics;
	diagnostics.Completed(timing, 405, false, true, false);
	diagnostics.Completed({ 0, 10, 20 }, 30, false, false, true);
	const auto stats = diagnostics.Take(410);
	ASSERT_EQUALS(uint64_t{ 2 }, stats.work.count);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.failed);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.deferred);
}

TEST(UploadReadDiagnostics, IdleClosesDoNotDistortReadLatency)
{
	UploadReadDiagnostics diagnostics;
	diagnostics.Completed({ 0, 5, 2005 }, 2010, true, false, false);
	const auto stats = diagnostics.Take(2020);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.work.count);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.close.count);
	ASSERT_EQUALS(uint64_t{ 2000 }, stats.close.maxMs);
}

TEST(UploadReadDiagnostics, PressureIsWallTimeNotSummedClientWaits)
{
	UploadReadDiagnostics diagnostics;
	diagnostics.EndSweep(0, false);
	diagnostics.Denied(UploadReadDiagnostics::Storage);
	diagnostics.Denied(UploadReadDiagnostics::Storage);
	diagnostics.Denied(UploadReadDiagnostics::ClientMemory);
	diagnostics.EndSweep(10, true);
	const auto first = diagnostics.Take(110);
	ASSERT_EQUALS(uint64_t{ 2 }, first.denials[UploadReadDiagnostics::Storage]);
	ASSERT_EQUALS(uint64_t{ 100 }, first.pressureMs[UploadReadDiagnostics::Storage]);
	ASSERT_EQUALS(uint64_t{ 100 }, first.pressureMs[UploadReadDiagnostics::ClientMemory]);
	ASSERT_EQUALS(uint64_t{ 100 }, first.pressureMs[UploadReadDiagnostics::Workers]);
	ASSERT_EQUALS(uint64_t{ 0 }, first.pressureMs[UploadReadDiagnostics::GlobalMemory]);
	// An interval reset keeps an ongoing stall visible, without double counting.
	const auto second = diagnostics.Take(210);
	ASSERT_EQUALS(uint64_t{ 0 }, second.denials[UploadReadDiagnostics::Storage]);
	ASSERT_EQUALS(uint64_t{ 100 }, second.pressureMs[UploadReadDiagnostics::Storage]);
	diagnostics.EndSweep(220, false);
	const auto recovered = diagnostics.Take(1000);
	ASSERT_EQUALS(uint64_t{ 10 }, recovered.pressureMs[UploadReadDiagnostics::Storage]);
	ASSERT_EQUALS(uint64_t{ 10 }, recovered.pressureMs[UploadReadDiagnostics::Workers]);
	ASSERT_EQUALS(uint64_t{ 0 }, diagnostics.Take(2000).pressureMs[UploadReadDiagnostics::Storage]);
}

TEST(UploadReadDiagnostics, MemoryReasonsChangeWithoutLeakingOldPressure)
{
	UploadReadDiagnostics diagnostics;
	diagnostics.Denied(UploadReadDiagnostics::ClientMemory);
	diagnostics.EndSweep(100, false);
	diagnostics.Denied(UploadReadDiagnostics::GlobalMemory);
	diagnostics.EndSweep(150, false);
	diagnostics.EndSweep(180, false);
	const auto stats = diagnostics.Take(200);
	ASSERT_EQUALS(uint64_t{ 50 }, stats.pressureMs[UploadReadDiagnostics::ClientMemory]);
	ASSERT_EQUALS(uint64_t{ 30 }, stats.pressureMs[UploadReadDiagnostics::GlobalMemory]);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.denials[UploadReadDiagnostics::ClientMemory]);
	ASSERT_EQUALS(uint64_t{ 1 }, stats.denials[UploadReadDiagnostics::GlobalMemory]);
}

TEST(UploadReadDiagnostics, DisabledTimingDoesNotReadClockOrPublishSamples)
{
	UploadReadTiming timing;
	timing.enabled = false;
	unsigned calls = 0;
	bool worked = false;
	timing.Run([&worked] { worked = true; }, [&calls] { return ++calls; });
	ASSERT_TRUE(worked);
	ASSERT_EQUALS(0u, calls);
	UploadReadDiagnostics diagnostics;
	diagnostics.Completed(timing, 100, false, true, true);
	diagnostics.Completed(timing, 100, true, false, false);
	const auto stats = diagnostics.Take(100);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.work.count);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.close.count);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.failed);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.deferred);
}
