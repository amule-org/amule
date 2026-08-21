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

#include "SharedFilesReloadLatch.h"

using namespace muleunit;

DECLARE_SIMPLE(SharedFilesReloadLatch)

TEST(SharedFilesReloadLatch, StartsIdle)
{
	CSharedFilesReloadLatch latch;
	ASSERT_TRUE(!latch.IsPending());
	ASSERT_TRUE(!latch.ShouldStartFromTick(/*walkRunning=*/false));
}

TEST(SharedFilesReloadLatch, RequestsCoalesce)
{
	// Ten EC callers before the next tick must produce one walk, not ten.
	CSharedFilesReloadLatch latch;
	for (int i = 0; i < 10; ++i) {
		latch.Request();
	}
	ASSERT_TRUE(latch.ShouldStartFromTick(false));
	ASSERT_TRUE(latch.BeginWalk());
	latch.EndWalk(true, /*aborted=*/false);
	ASSERT_TRUE(!latch.IsPending());
	ASSERT_TRUE(!latch.ShouldStartFromTick(false));
}

TEST(SharedFilesReloadLatch, TickDoesNotStartWhileAWalkRuns)
{
	// A request arriving mid-walk waits for a later tick rather than nesting.
	CSharedFilesReloadLatch latch;
	latch.Request();
	ASSERT_TRUE(!latch.ShouldStartFromTick(/*walkRunning=*/true));
	ASSERT_TRUE(latch.ShouldStartFromTick(/*walkRunning=*/false));
}

TEST(SharedFilesReloadLatch, RequestDuringAWalkSurvivesIt)
{
	// The walk in flight is already past the files the new request is about,
	// so completing must not consume it. This is why BeginWalk() takes the
	// request rather than EndWalk() clearing the flag.
	CSharedFilesReloadLatch latch;
	latch.Request();
	const bool serving = latch.BeginWalk();
	ASSERT_TRUE(serving);
	ASSERT_TRUE(!latch.IsPending());

	latch.Request(); // arrives while the walk is running
	latch.EndWalk(serving, /*aborted=*/false);

	ASSERT_TRUE(latch.IsPending());
	ASSERT_TRUE(latch.ShouldStartFromTick(false));
}

TEST(SharedFilesReloadLatch, AbortedWalkGivesTheRequestBack)
{
	// A cancelled scan leaves a partial share list and satisfies nothing.
	CSharedFilesReloadLatch latch;
	latch.Request();
	const bool serving = latch.BeginWalk();
	latch.EndWalk(serving, /*aborted=*/true);
	ASSERT_TRUE(latch.IsPending());
}

TEST(SharedFilesReloadLatch, AbortedWalkWithNoRequestInventsNone)
{
	// A user-initiated walk that nobody asked for, then cancelled, must not
	// leave a request behind for the next tick to act on.
	CSharedFilesReloadLatch latch;
	const bool serving = latch.BeginWalk();
	ASSERT_TRUE(!serving);
	latch.EndWalk(serving, /*aborted=*/true);
	ASSERT_TRUE(!latch.IsPending());
}

TEST(SharedFilesReloadLatch, AbortedWalkKeepsAMidWalkRequestOnce)
{
	// Both rules at once: the walk aborts (its own request comes back) while
	// another request arrived mid-walk. One flag, so one walk is owed, not two
	// -- the next walk covers both.
	CSharedFilesReloadLatch latch;
	latch.Request();
	const bool serving = latch.BeginWalk();
	latch.Request();
	latch.EndWalk(serving, /*aborted=*/true);
	ASSERT_TRUE(latch.IsPending());

	const bool nextServing = latch.BeginWalk();
	ASSERT_TRUE(nextServing);
	latch.EndWalk(nextServing, false);
	ASSERT_TRUE(!latch.IsPending());
}

TEST(SharedFilesReloadLatch, CompletedWalkWithNoRequestChangesNothing)
{
	CSharedFilesReloadLatch latch;
	const bool serving = latch.BeginWalk();
	latch.EndWalk(serving, false);
	ASSERT_TRUE(!latch.IsPending());
}
