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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include "UploadReadDiagnostics.h"
#include <limits>

using namespace muleunit;
using namespace UploadReadDiagnostics;
DECLARE_SIMPLE(UploadReadDiagnostics)

TEST(UploadReadDiagnostics, HistogramBoundsAndReset)
{
	Histogram histogram;
	ASSERT_EQUALS(uint64_t{ 0 }, histogram.Median());
	for (uint64_t value : { 0, 1, 3, 8, 9 }) {
		histogram.Add(value);
	}
	ASSERT_EQUALS(uint64_t{ 4 }, histogram.Median());
	ASSERT_EQUALS(uint64_t{ 9 }, histogram.maximum);
	ASSERT_EQUALS(uint64_t{ 5 }, histogram.count);
	histogram = {};
	ASSERT_EQUALS(uint64_t{ 0 }, histogram.count);
	histogram.Add(std::numeric_limits<uint64_t>::max());
	ASSERT_EQUALS(std::numeric_limits<uint64_t>::max(), histogram.Median());
}

TEST(UploadReadDiagnostics, EverySampleHasOneState)
{
	struct Case
	{
		bool pending;
		bool requests;
		bool atTarget;
		bool queued;
		SlotState state;
	};
	const Case cases[] = { { false, false, false, false, SlotState::Peer },
		{ false, false, false, true, SlotState::Sending },
		{ false, false, true, true, SlotState::Sending },
		{ false, true, false, false, SlotState::Storage },
		{ false, true, false, true, SlotState::Storage },
		{ false, true, true, true, SlotState::Receiver },
		{ true, false, false, false, SlotState::Reading },
		{ true, false, false, true, SlotState::Reading },
		{ true, false, true, true, SlotState::Reading },
		{ true, true, false, false, SlotState::Reading },
		{ true, true, false, true, SlotState::Reading },
		{ true, true, true, true, SlotState::Reading } };
	for (const auto &sample : cases) {
		ASSERT_TRUE(Classify(sample.pending, sample.requests, sample.atTarget, sample.queued) ==
			    sample.state);
	}
}

TEST(UploadReadDiagnostics, SendStatsTimeOnlyRefusedWrites)
{
	SendStats stats;
	stats.Resumed(5, 10); // a wake with no refused write times nothing
	ASSERT_EQUALS(uint64_t{ 0 }, stats.busyMaximum);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.wakeMaximum);
	stats.Refused(100);
	stats.Refused(150); // still the same wait
	stats.Resumed(100, 400);
	ASSERT_EQUALS(uint64_t{ 2 }, stats.refusedWrites);
	ASSERT_EQUALS(uint64_t{ 300 }, stats.busyMaximum);
	ASSERT_EQUALS(uint64_t{ 100 }, stats.wakeMaximum);
	stats.Refused(500);
	stats.Resumed(0, 510); // a shorter wait keeps the maximum
	ASSERT_EQUALS(uint64_t{ 300 }, stats.busyMaximum);
	ASSERT_EQUALS(uint64_t{ 100 }, stats.wakeMaximum);
}

TEST(UploadReadDiagnostics, SendStatsTakeResetsAndKeepsAnOngoingWait)
{
	SendStats stats;
	stats.sentBytes = 7;
	stats.lastSent = 90;
	stats.Refused(100);
	SendStats report = stats.Take(160);
	ASSERT_EQUALS(uint64_t{ 7 }, report.sentBytes);
	ASSERT_EQUALS(uint64_t{ 60 }, report.busyMaximum);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.sentBytes);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.refusedWrites);
	ASSERT_EQUALS(uint64_t{ 90 }, stats.lastSent);
	stats.Resumed(10, 200);
	report = stats.Take(200);
	ASSERT_EQUALS(uint64_t{ 100 }, report.busyMaximum);
	ASSERT_EQUALS(uint64_t{ 10 }, report.wakeMaximum);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.busySince);
}

TEST(UploadReadDiagnostics, ElapsedTimeNeverUnderflows)
{
	ASSERT_EQUALS(uint64_t{ 40 }, ElapsedSince(100, 60));
	ASSERT_EQUALS(uint64_t{ 0 }, ElapsedSince(100, 100));
	ASSERT_EQUALS(uint64_t{ 0 }, ElapsedSince(100, 101));

	SendStats stats;
	stats.Refused(101);
	ASSERT_EQUALS(uint64_t{ 0 }, stats.Take(100).busyMaximum);
}

TEST(UploadReadDiagnostics, HistogramCoversEveryPowerOfTwoBoundary)
{
	for (unsigned bit = 0; bit < 30; ++bit) {
		const uint64_t bound = uint64_t{ 1 } << bit;
		Histogram exact;
		exact.Add(bound);
		ASSERT_EQUALS(bound, exact.Median());
		if (bound > 1) {
			Histogram lower;
			lower.Add(bound - 1);
			ASSERT_EQUALS(bound == 2 ? uint64_t{ 1 } : bound, lower.Median());
		}
		if (bit < 29) {
			Histogram upper;
			upper.Add(bound + 1);
			ASSERT_EQUALS(bound << 1, upper.Median());
		}
	}
	// Above the last bound, the median is the exact maximum.
	Histogram overflow;
	overflow.Add((uint64_t{ 1 } << 29) + 1);
	ASSERT_EQUALS((uint64_t{ 1 } << 29) + 1, overflow.Median());
}

TEST(UploadReadDiagnostics, DispatchWaitResetsForChangedRequestsAndInactiveSlots)
{
	DispatchWait wait;
	std::array<uint8_t, 16> first{};
	auto second = first;
	second[0] = 1;
	wait.Observe(1, 0, 10, first.data(), 0);
	ASSERT_EQUALS(uint64_t{ 20 }, wait.Elapsed(20));
	wait.Observe(1, 0, 10, first.data(), 20);
	ASSERT_EQUALS(uint64_t{ 30 }, wait.Elapsed(30));
	wait.Observe(1, 0, 10, second.data(), 30);
	ASSERT_EQUALS(uint64_t{ 10 }, wait.Elapsed(40));
	wait.Observe(2, 0, 10, second.data(), 40);
	ASSERT_EQUALS(uint64_t{ 10 }, wait.Elapsed(50));
	wait.Observe(2, 10, 20, second.data(), 50);
	ASSERT_EQUALS(uint64_t{ 10 }, wait.Elapsed(60));
	wait.Reset();
	ASSERT_EQUALS(uint64_t{ 0 }, wait.Elapsed(70));
	wait.Observe(2, 10, 20, second.data(), 80);
	ASSERT_EQUALS(uint64_t{ 5 }, wait.Elapsed(85));
}
