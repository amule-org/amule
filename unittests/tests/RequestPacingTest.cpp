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
#include "RequestPacing.h"

using namespace muleunit;

DECLARE_SIMPLE(RequestPacing)

namespace
{
const uint64_t kReask = 15 * 60 * 1000; // the default source reask time
const uint64_t kNow = 100 * 60 * 1000;

// When CPartFile::Process() asks again: lastAsked 0 means its next tick.
uint64_t AskAt(uint64_t lastAsked)
{
	return lastAsked ? lastAsked + kReask : kNow;
}
} // namespace

TEST(RequestPacing, NoEarlierRequestAsksNow)
{
	ASSERT_EQUALS(0u, PacedLastAskedTime(kNow, 0, kReask));
}

TEST(RequestPacing, OldRequestAsksNow)
{
	ASSERT_EQUALS(0u, PacedLastAskedTime(kNow, kNow - MIN_REQUESTTIME, kReask));
	ASSERT_EQUALS(0u, PacedLastAskedTime(kNow, kNow - 2 * MIN_REQUESTTIME, kReask));
}

TEST(RequestPacing, RecentRequestWaitsForMinRequestTime)
{
	const uint64_t last = kNow - 60 * 1000;
	ASSERT_EQUALS(last + MIN_REQUESTTIME, AskAt(PacedLastAskedTime(kNow, last, kReask)));
}

TEST(RequestPacing, NeverLaterThanAPlainReask)
{
	// The paced ask is always sooner than the reask after the request it waits for.
	const uint64_t last = kNow - 1;
	ASSERT_TRUE(AskAt(PacedLastAskedTime(kNow, last, kReask)) < last + kReask);
}

TEST(RequestPacing, ReaskShorterThanMinRequestTime)
{
	const uint64_t shortReask = MIN_REQUESTTIME / 2;
	const uint64_t last = kNow - 1000;
	const uint64_t lastAsked = PacedLastAskedTime(kNow, last, shortReask);
	ASSERT_EQUALS(last + MIN_REQUESTTIME, lastAsked + shortReask);
}

TEST(RequestPacing, EarlyUptimeDoesNotWrap)
{
	// Right after boot the tick count can be below the reask time.
	ASSERT_EQUALS(1u, PacedLastAskedTime(1000, 500, 10 * MIN_REQUESTTIME));
}
