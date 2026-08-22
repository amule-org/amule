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

// CKnownFile::UpdatePartsInfo() recomputes the complete-sources estimate at
// most once every 60 s, with one exception: the upload list going empty. That
// transition cannot be deferred, because every caller is peer-driven and there
// is no periodic sweep -- a throttled call there is the last one the file ever
// gets, and the counts keep their values until aMule restarts (issue #1050).
//
// The bypass is guarded so a file already settled at 0 does not re-enter the
// recompute forever. #1052 guarded it on the scalar alone, which was too
// narrow: Hi is a percentile of the peers' self-reported counts and is only
// floored at the scalar, never tied to it, so scalar 0 with Hi non-zero is a
// reachable and in fact ordinary state -- any peer without extended requests
// v2 contributes 0. Since the desktop column and the Web UI detail panel both
// render "< Hi" whenever Lo is 0, that stale Hi is exactly what the user sees
// (issue #1065).
//
// The predicate lives in its own header precisely so this can be tested:
// CKnownFile reaches theApp and cannot be linked into a unit test.

#include <muleunit/test.h>

#include <CompleteSourcesThrottle.h>

using namespace muleunit;

DECLARE_SIMPLE(CompleteSourcesThrottle)

// The bypass only ever applies when there is nobody left to ask.
TEST(CompleteSourcesThrottle, PeersStillPresentNeverBypasses)
{
	ASSERT_FALSE(CompleteSourcesNeedRecompute(false, 0, 0, 0));
	ASSERT_FALSE(CompleteSourcesNeedRecompute(false, 5, 5, 5));
	ASSERT_FALSE(CompleteSourcesNeedRecompute(false, 0, 0, 5));
}

// Criterion 4: a file settled at zero must short-circuit, or every later call
// on every idle shared file re-runs the full recompute.
TEST(CompleteSourcesThrottle, AllZeroStaysSettled)
{
	ASSERT_FALSE(CompleteSourcesNeedRecompute(true, 0, 0, 0));
}

// The case #1052 already handled: a non-zero scalar must still recompute.
TEST(CompleteSourcesThrottle, NonZeroScalarBypasses)
{
	ASSERT_TRUE(CompleteSourcesNeedRecompute(true, 5, 5, 5));
	ASSERT_TRUE(CompleteSourcesNeedRecompute(true, 1, 0, 0));
}

// The regression #1065 is about, and the reason the scalar-only guard was
// wrong. These are the states the estimation actually produces for a mixed
// peer population, taken from the issue's sweep: two peers, one of them
// reporting a non-zero count, lands on scalar 0 / Lo 0 / Hi N.
TEST(CompleteSourcesThrottle, StaleHighAloneStillBypasses)
{
	ASSERT_TRUE(CompleteSourcesNeedRecompute(true, 0, 0, 1));
	ASSERT_TRUE(CompleteSourcesNeedRecompute(true, 0, 0, 5));
}

// Lo cannot diverge from the scalar in either estimation branch -- n < 20
// assigns the scalar from Lo, n >= 20 floors the scalar at Lo -- so this state
// is not reachable today. Pinned anyway: the guard is written to survive the
// heuristic changing, and this is the assertion that would notice if it did.
TEST(CompleteSourcesThrottle, StaleLowAloneStillBypasses)
{
	ASSERT_TRUE(CompleteSourcesNeedRecompute(true, 0, 3, 0));
}
