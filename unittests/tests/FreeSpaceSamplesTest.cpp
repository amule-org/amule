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
#include "FreeSpaceSamples.h"
#include <protocol/ed2k/Constants.h>

using namespace muleunit;

namespace
{
const uint64 kMB = 1024 * 1024;
} // namespace

DECLARE_SIMPLE(FreeSpaceAllows);

TEST(FreeSpaceAllows, NeverBelowOnePart)
{
	ASSERT_FALSE(FreeSpaceAllows(PARTSIZE - 1, 0, false, 0));
	ASSERT_TRUE(FreeSpaceAllows(PARTSIZE, 0, false, 0));
	// With the user's limit off, the bytes to write do not matter.
	ASSERT_TRUE(FreeSpaceAllows(PARTSIZE, 100 * kMB, false, 0));
}

TEST(FreeSpaceAllows, KeepsTheUserLimitAfterTheWrite)
{
	const sint64 free = 100 * kMB;
	ASSERT_TRUE(FreeSpaceAllows(free, 40 * kMB, true, 60 * kMB));
	ASSERT_FALSE(FreeSpaceAllows(free, 40 * kMB + 1, true, 60 * kMB));
}

TEST(FreeSpaceAllows, UnknownFails)
{
	ASSERT_FALSE(FreeSpaceAllows(-1, 0, false, 0));
}

DECLARE_SIMPLE(CFreeSpaceSamples);

TEST(CFreeSpaceSamples, ReturnsARecentSampleOnly)
{
	CFreeSpaceSamples samples;
	const CPath dir("/some/temp");
	ASSERT_EQUALS(-1, samples.Get(dir, 1000, 30000)); // never sampled

	samples.Publish(dir, 500 * kMB, 1000);
	ASSERT_EQUALS((sint64)(500 * kMB), samples.Get(dir, 31000, 30000));
	// Stuck on the mount: too old to act on.
	ASSERT_EQUALS(-1, samples.Get(dir, 31001, 30000));
	ASSERT_EQUALS(-1, samples.Get(CPath("/other"), 1000, 30000));
}

TEST(CFreeSpaceSamples, IgnoresASampleTakenBeforeNotBefore)
{
	CFreeSpaceSamples samples;
	const CPath dir("/some/temp");
	samples.Publish(dir, 500 * kMB, 1000);
	// The file was paused for space at 1500: a probe from 1000 can predate what filled the disk.
	ASSERT_EQUALS(-1, samples.Get(dir, 2000, 30000, 1500));
	samples.Publish(dir, 80 * kMB, 1600);
	ASSERT_EQUALS((sint64)(80 * kMB), samples.Get(dir, 2000, 30000, 1500));
}

TEST(CFreeSpaceSamples, AFailedProbeIsUnknown)
{
	CFreeSpaceSamples samples;
	const CPath dir("/gone");
	samples.Publish(dir, -1, 1000);
	ASSERT_EQUALS(-1, samples.Get(dir, 1000, 30000));
}

TEST(CFreeSpaceSamples, RetainDropsOtherDirectories)
{
	CFreeSpaceSamples samples;
	const CPath kept("/temp");
	const CPath old("/old-temp");
	samples.Publish(kept, 10 * kMB, 1000);
	samples.Publish(old, 20 * kMB, 1000);
	samples.Retain({ kept });
	ASSERT_EQUALS((sint64)(10 * kMB), samples.Get(kept, 1000, 30000));
	ASSERT_EQUALS(-1, samples.Get(old, 1000, 30000));
}
