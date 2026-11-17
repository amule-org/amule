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
#include "SourceRetryPolicy.h"
#include "RequestPacing.h"
using namespace muleunit;
using namespace SourceRetryPolicy;
DECLARE_SIMPLE(SourceRetryPolicy)

TEST(SourceRetryPolicy, KeepsProtocolReaskIntervalsAndStrictBoundary)
{
	// Exercise startup and a clock beyond the 32-bit millisecond range.
	for (const uint64_t lastAsked : { uint64_t{ 1 }, uint64_t{ 1 } << 32 }) {
		for (const uint64_t reask : { 15 * 60000, 29 * 60000, 60 * 60000 }) {
			for (const uint64_t delay : { reask, reask - 20000, reask * 2 }) {
				const uint64_t deadline = lastAsked + delay;
				ASSERT_FALSE(IsDue(lastAsked, lastAsked, delay));
				ASSERT_FALSE(IsDue(deadline - 1, lastAsked, delay));
				ASSERT_FALSE(IsDue(deadline, lastAsked, delay));
				ASSERT_TRUE(IsDue(deadline + 1, lastAsked, delay));
				ASSERT_TRUE(IsDue(0, 0, delay));
				ASSERT_TRUE(IsDue(lastAsked, 0, delay));
			}
		}
	}
}
TEST(SourceRetryPolicy, KeepsGlobalAndPerFileDeadSourceExclusions)
{
	ASSERT_EQUALS(uint64_t{ 30 * 60000 }, DeadSourceDelay(true, false));
	ASSERT_EQUALS(uint64_t{ 45 * 60000 }, DeadSourceDelay(true, true));
	ASSERT_EQUALS(uint64_t{ 45 * 60000 }, DeadSourceDelay(false, false));
	ASSERT_EQUALS(uint64_t{ 60 * 60000 }, DeadSourceDelay(false, true));
	const uint64_t now = uint64_t{ 1 } << 32;
	ASSERT_EQUALS(now + 30 * 60000, DeadSourceDeadline(now, true, false));
	ASSERT_EQUALS(now + 45 * 60000, DeadSourceDeadline(now, true, true));
	ASSERT_EQUALS(now + 45 * 60000, DeadSourceDeadline(now, false, false));
	ASSERT_EQUALS(now + 60 * 60000, DeadSourceDeadline(now, false, true));
}
TEST(SourceRetryPolicy, PacedRetryDoesNotBypassAggressiveRequestGuard)
{
	for (const uint64_t reask : { 15 * 60000, 29 * 60000, 60 * 60000 }) {
		for (const uint64_t lastSent : { uint64_t{ 500 }, uint64_t{ 1 } << 32 }) {
			const uint64_t now = lastSent + 1000;
			const uint64_t lastAsked = PacedLastAskedTime(now, lastSent, reask);
			ASSERT_FALSE(IsDue(now, lastAsked, reask));
			ASSERT_FALSE(IsDue(lastSent + MIN_REQUESTTIME - 1, lastAsked, reask));
			ASSERT_FALSE(IsDue(lastSent + MIN_REQUESTTIME, lastAsked, reask));
			// At early uptime, RequestPacing uses 1 when the synthetic timestamp
			// would precede boot. The retry must still wait, even past the minimum.
			const uint64_t deadline = lastAsked + reask;
			ASSERT_EQUALS(lastSent < reask ? reask + 1 : lastSent + MIN_REQUESTTIME, deadline);
			ASSERT_FALSE(IsDue(deadline, lastAsked, reask));
			ASSERT_TRUE(IsDue(deadline + 1, lastAsked, reask));
			ASSERT_TRUE(IsDue(now, PacedLastAskedTime(now, 0, reask), reask));
			ASSERT_TRUE(IsDue(lastSent + MIN_REQUESTTIME,
				PacedLastAskedTime(lastSent + MIN_REQUESTTIME, lastSent, reask),
				reask));
		}
	}
}
TEST(SourceRetryPolicy, SourceWaitingForItsAskSurvivesDisconnect)
{
	// A source waiting for its paced ask is in DS_NONE. An idle socket closing in that wait
	// must not drop it.
	ASSERT_TRUE(KeepOnDisconnect(DS_NONE, true));
	ASSERT_FALSE(KeepOnDisconnect(DS_NONE, false));
	for (const uint8_t state : { DS_ONQUEUE, DS_TOOMANYCONNS, DS_NONEEDEDPARTS, DS_LOWTOLOWIP }) {
		ASSERT_TRUE(KeepOnDisconnect(state, true));
		ASSERT_TRUE(KeepOnDisconnect(state, false));
	}
	// The other states are still given up.
	for (const uint8_t state : { DS_CONNECTING, DS_CONNECTED, DS_WAITCALLBACK, DS_ERROR, DS_BANNED }) {
		ASSERT_FALSE(KeepOnDisconnect(state, true));
	}
}
