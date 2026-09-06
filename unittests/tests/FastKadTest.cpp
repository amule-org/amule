//								-*- C++ -*-
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include <muleunit/test.h>
#include <kademlia/net/FastKad.h>

using namespace muleunit;
using Kademlia::CFastKad;

DECLARE_SIMPLE(FastKad)

// Wall-clock zero for these tests; every call passes "now" explicitly, so no
// test here depends on a real clock.
static const uint64_t T0 = 1000000;

// Every expected ceiling below is a literal worked out by hand from the
// documented estimator -- mean + 2 * stddev + margin, with both moments divided
// by the full window and every unfilled slot counted as the default response
// time -- and NOT by reading CFastKad's own constants back. Reading them here
// would make the assertions restate the implementation instead of pinning its
// output, and would stay green through an arithmetic change.
//
// The literals therefore assume the window and the two reference times below.
// If any of them ever changes, ConstantsTheseTestsAssume fails first and says
// so, instead of the arithmetic tests quietly re-deriving a new answer.
static const unsigned WINDOW = 100;
static const unsigned DEFAULT_MS = 1000;
static const unsigned CAP_MS = 3000;

TEST(FastKad, ConstantsTheseTestsAssume)
{
	ASSERT_EQUALS(WINDOW, (unsigned)CFastKad::MAX_SAMPLES);
	ASSERT_EQUALS(DEFAULT_MS, (unsigned)CFastKad::DEFAULT_RESPONSE_TIME_MS);
	ASSERT_EQUALS(CAP_MS, (unsigned)CFastKad::MAX_RESPONSE_TIME_MS);
	ASSERT_EQUALS(100u, (unsigned)CFastKad::RESPONSE_TIME_MARGIN_MS);
	ASSERT_EQUALS(300000u, (unsigned)CFastKad::MIN_EVICTION_AGE_MS);
}

TEST(FastKad, ColdStartIsTheDefaultResponseTime)
{
	CFastKad kad;
	ASSERT_EQUALS(0u, (unsigned)kad.GetSampleCount());
	// With no sample at all the estimate must be usable, not zero: a zero
	// timeout would drop every contact the instant it was asked.
	ASSERT_EQUALS(1000u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, OneFastSampleMovesTheCeilingByTheDocumentedAmount)
{
	CFastKad kad;
	kad.AddResponseTime(0x01020304, 500, T0);
	ASSERT_EQUALS(1u, (unsigned)kad.GetSampleCount());

	// One sample of 500 ms in a 100-slot window whose other 99 slots count
	// as 1000 ms:
	//   mean     = (500 + 99 * 1000) / 100                      = 995
	//   variance = ((500-995)^2 + 99 * (1000-995)^2) / 99       = 2500
	//   estimate = 995 + 2 * 50 + 100                           = 1195
	// A single fast answer therefore cannot collapse the ceiling: it moves
	// it by 5 ms shy of the margin, not by half the default.
	ASSERT_EQUALS(1195u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, AKnownSeriesProducesTheHandComputedCeiling)
{
	CFastKad kad;
	// Four addresses, one answer each, at 200/400/600/800 ms.
	//   mean     = (2000 + 96 * 1000) / 100                     = 980
	//   variance = (780^2 + 580^2 + 380^2 + 180^2
	//               + 96 * 20^2) / 99                           = 11717.171...
	//   stddev   = 108.2458...
	//   estimate = 980 + 216.4917... + 100                      = 1296.4917...
	// truncated to 1296.
	kad.AddResponseTime(0x0A000001, 200, T0);
	kad.AddResponseTime(0x0A000002, 400, T0 + 1);
	kad.AddResponseTime(0x0A000003, 600, T0 + 2);
	kad.AddResponseTime(0x0A000004, 800, T0 + 3);

	ASSERT_EQUALS(4u, (unsigned)kad.GetSampleCount());
	ASSERT_EQUALS(1296u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, AFullWindowOfEqualSamplesIsThatValuePlusTheMargin)
{
	CFastKad kad;
	// A full window removes the unfilled-slot bias entirely, and equal
	// samples have zero variance, so the estimate is exactly the sample
	// value plus the margin. This is the tightest available check on the
	// mean, on the margin, and on the window being genuinely full.
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0B000000 + i, 200, T0);
	}
	ASSERT_EQUALS(WINDOW, (unsigned)kad.GetSampleCount());
	ASSERT_EQUALS(300u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, AWindowOfInstantAnswersLeavesOnlyTheMargin)
{
	CFastKad kad;
	// The pathological floor: a whole window of instant answers. Mean and
	// variance are both zero, so only the margin is left.
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0C000000 + i, 0, T0);
	}
	ASSERT_EQUALS(100u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, TheEstimateIsCappedAtTheDocumentedMaximum)
{
	CFastKad kad;
	// Half the nodes answer instantly, half take ten minutes. The raw
	// formula yields roughly 903122 ms, which would stall every search;
	// the cap is what makes that survivable.
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0D000000 + i, (i % 2) ? 1 : 600000, T0);
	}
	ASSERT_EQUALS(3000u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, ARisingDistributionRaisesTheCeilingToTheHandComputedValue)
{
	CFastKad kad;
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0E000000 + i, 200, T0);
	}
	ASSERT_EQUALS(300u, (unsigned)kad.GetEstMaxResponseTime());

	// The same hundred addresses now answer in 2500 ms each. Still a full
	// window, still zero variance: 2500 + 100.
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0E000000 + i, 2500, T0 + 1);
	}
	ASSERT_EQUALS(WINDOW, (unsigned)kad.GetSampleCount());
	ASSERT_EQUALS(2600u, (unsigned)kad.GetEstMaxResponseTime());

	// And back down again, to prove the window replaces rather than
	// accumulates.
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0E000000 + i, 120, T0 + 2);
	}
	ASSERT_EQUALS(220u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, TheWindowIsBoundedAndFreshSamplesAreNotEvicted)
{
	CFastKad kad;
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x0F000000 + i, 200, T0);
	}
	ASSERT_EQUALS(300u, (unsigned)kad.GetEstMaxResponseTime());

	// Every existing sample is younger than the eviction age, so a flood of
	// new addresses must be dropped rather than churn the window. Memory use
	// stops growing AND the estimate is untouched -- the second half is what
	// distinguishes "rejected" from "admitted and coincidentally equal".
	for (unsigned i = 0; i < WINDOW * 2; ++i) {
		kad.AddResponseTime(0xA0000000 + i, 2500, T0 + 1);
	}
	ASSERT_EQUALS(WINDOW, (unsigned)kad.GetSampleCount());
	ASSERT_EQUALS(300u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, AStaleSampleIsEvictedToMakeRoomForANewAddress)
{
	CFastKad kad;
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x10000000 + i, 200, T0);
	}

	// Past the eviction age the window is all stale, so one entry gives way.
	// The window is then 99 samples of 200 ms plus one of 2500 ms:
	//   mean     = (99 * 200 + 2500) / 100                      = 223
	//   variance = (99 * 23^2 + 2277^2) / 99                    = 52900
	//   estimate = 223 + 2 * 230 + 100                          = 783
	// Nothing but a genuine eviction-and-insert produces 783.
	kad.AddResponseTime(0xFFFFFFFF, 2500, T0 + 300001);
	ASSERT_EQUALS(WINDOW, (unsigned)kad.GetSampleCount());
	ASSERT_EQUALS(783u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, RepeatedSamplesFromOneAddressReplaceRatherThanAccumulate)
{
	CFastKad kad;
	for (unsigned i = 0; i < 10; ++i) {
		kad.AddResponseTime(0x7F000001, 100 + i, T0 + i);
	}
	ASSERT_EQUALS(1u, (unsigned)kad.GetSampleCount());

	// Only the last value, 109 ms, survives:
	//   mean     = (109 + 99 * 1000) / 100                      = 991.09
	//   variance = ((109-991.09)^2 + 99 * 8.91^2) / 99          = 7938.81
	//   estimate = 991.09 + 2 * 89.1 + 100                      = 1269.29
	// truncated to 1269. Accumulating the ten samples instead would leave a
	// different mean and a different answer.
	ASSERT_EQUALS(1269u, (unsigned)kad.GetEstMaxResponseTime());
}

TEST(FastKad, ClearReturnsToColdStart)
{
	CFastKad kad;
	for (unsigned i = 0; i < WINDOW; ++i) {
		kad.AddResponseTime(0x11000000 + i, 2500, T0);
	}
	ASSERT_EQUALS(2600u, (unsigned)kad.GetEstMaxResponseTime());

	kad.Clear();
	ASSERT_EQUALS(0u, (unsigned)kad.GetSampleCount());
	ASSERT_EQUALS(1000u, (unsigned)kad.GetEstMaxResponseTime());
}
