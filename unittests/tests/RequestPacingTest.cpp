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

TEST(RequestPacing, AggressiveScoreFollowsCheckForAggressive)
{
	ASSERT_EQUALS(0u, AggressiveScoreAfter(0, kNow, 0)); // no earlier request
	ASSERT_EQUALS(3u, AggressiveScoreAfter(0, kNow, kNow - 1));
	ASSERT_EQUALS(6u, AggressiveScoreAfter(3, kNow, kNow - MIN_REQUESTTIME + 1));
	ASSERT_EQUALS(2u, AggressiveScoreAfter(3, kNow, kNow - MIN_REQUESTTIME));
	ASSERT_EQUALS(0u, AggressiveScoreAfter(0, kNow, kNow - MIN_REQUESTTIME));
}

TEST(RequestPacing, RepeatedEarlyAsksStayBelowTheBan)
{
	// An early ask after every polite request that MayAskEarly allows, as repeated rehash
	// failures cause. One early ask per MIN_REQUESTTIME reached aMule's ban at the fifth.
	unsigned score = 0;
	unsigned earlyAsks = 0;
	uint64_t last = kNow;
	for (int polite = 0; polite < 99; ++polite) {
		const uint64_t now = last + MIN_REQUESTTIME;
		score = AggressiveScoreAfter(score, now, last);
		last = now;
		if (MayAskEarly(score)) {
			score = AggressiveScoreAfter(score, last + 1000, last);
			last += 1000;
			++earlyAsks;
		}
		ASSERT_TRUE(score <= 3);
	}
	// Three polite requests clear each early ask.
	ASSERT_EQUALS(33u, earlyAsks);
}
