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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include <muleunit/test.h>
#include "SharedFileVerifySchedule.h"
#include <limits>

using namespace muleunit;
DECLARE_SIMPLE(SharedFileVerifySchedule)

TEST(SharedFileVerifySchedule, DisabledAndNegativeIntervalsNeverRun)
{
	CSharedFileVerifySchedule schedule;
	ASSERT_FALSE(schedule.IsDue(0, 0, 1000000, 0));
	ASSERT_FALSE(schedule.IsDue(-1, 0, 1000000, 0));
}

TEST(SharedFileVerifySchedule, NeverVerifiedAndExactDueBoundary)
{
	CSharedFileVerifySchedule schedule;
	ASSERT_TRUE(schedule.IsDue(7, 0, 1000000, 0));
	ASSERT_FALSE(schedule.IsDue(7, 1000000, 1000000 + 7 * 86400 - 1, 0));
	ASSERT_TRUE(schedule.IsDue(7, 1000000, 1000000 + 7 * 86400, 0));
}

TEST(SharedFileVerifySchedule, FailedAttemptAndDelayedEventAreThrottled)
{
	CSharedFileVerifySchedule schedule;
	schedule.OnQueued(0);
	ASSERT_FALSE(schedule.IsDue(1, 0, 1000000, 1));
	ASSERT_FALSE(schedule.IsDue(1, 1, 1000000, 86400000 - 1));
	ASSERT_TRUE(schedule.IsDue(1, 0, 1000000, 86400000));
}

TEST(SharedFileVerifySchedule, PersistedSuccessDefersNextCheckUntilCompletionDate)
{
	CSharedFileVerifySchedule schedule;
	schedule.OnQueued(1000);
	ASSERT_FALSE(schedule.IsDue(1, 1000100, 1000000 + 86400, 86401000));
	ASSERT_TRUE(schedule.IsDue(1, 1000100, 1000100 + 86400, 86501000));
}

TEST(SharedFileVerifySchedule, WallClockRollbackCannotCauseARequestStorm)
{
	CSharedFileVerifySchedule schedule;
	ASSERT_TRUE(schedule.IsDue(1, 2000000, 1000000, 0));
	schedule.OnQueued(100);
	ASSERT_FALSE(schedule.IsDue(1, 2000000, 1, 101));
	ASSERT_TRUE(schedule.IsDue(1, 2000000, 1, 86400100));
}

TEST(SharedFileVerifySchedule, LargeIntervalsDoNotOverflow)
{
	CSharedFileVerifySchedule schedule;
	const int days = std::numeric_limits<int>::max();
	const uint64 seconds = static_cast<uint64>(days) * 86400;
	ASSERT_FALSE(schedule.IsDue(days, 1, seconds, 0));
	ASSERT_TRUE(schedule.IsDue(days, 1, seconds + 1, 0));
	schedule.OnQueued(1);
	ASSERT_FALSE(schedule.IsDue(days, 0, seconds, seconds * 1000));
	ASSERT_TRUE(schedule.IsDue(days, 0, seconds, seconds * 1000 + 1));
}

TEST(SharedFileVerifySchedule, RestartWithoutSuccessPermitsRetry)
{
	CSharedFileVerifySchedule oldSession;
	oldSession.OnQueued(500);
	ASSERT_FALSE(oldSession.IsDue(1, 0, 1000000, 501));
	CSharedFileVerifySchedule newSession;
	ASSERT_TRUE(newSession.IsDue(1, 0, 1000000, 0));
	ASSERT_FALSE(newSession.IsDue(1, 1000000, 1000001, 0));
}
