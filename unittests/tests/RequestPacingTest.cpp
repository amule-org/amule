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
#include "SourceRetryPolicy.h"

#include <algorithm>

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

TEST(RequestPacing, NextAskTickFollowsProcess)
{
	ASSERT_EQUALS(0u, NextAskTick(0, kReask, kNow)); // asked on the next tick
	ASSERT_EQUALS(kNow + 1000, NextAskTick(kNow + 1000 - kReask, kReask, kNow));
	ASSERT_EQUALS(0u, NextAskTick(kNow - kReask, kReask, kNow)); // due now
	// AskAgainSoon() waits for the request before it to stop being early.
	const uint64_t last = kNow - 60 * 1000;
	ASSERT_EQUALS(
		last + MIN_REQUESTTIME, NextAskTick(PacedLastAskedTime(kNow, last, kReask), kReask, kNow));
}

TEST(RequestPacing, TickClockOffsetFollowsAClockStep)
{
	const int64_t offset = 1783675590000;
	ASSERT_EQUALS(offset, TickClockOffset(0, offset));                             // none yet
	ASSERT_EQUALS(offset, TickClockOffset(offset, offset + CLOCK_STEP_TOLERANCE)); // jitter
	ASSERT_EQUALS(offset, TickClockOffset(offset, offset - CLOCK_STEP_TOLERANCE));
	// NTP setting a clock that started in 1970.
	ASSERT_EQUALS(offset, TickClockOffset(5000, offset));
	ASSERT_EQUALS(offset - 3600000, TickClockOffset(offset, offset - 3600000));
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
	// An early ask after every polite request where the budget allows one, as repeated rehash
	// failures cause. One early ask per MIN_REQUESTTIME reached aMule's ban at the fifth.
	unsigned score = 0;
	unsigned earlyAsks = 0;
	uint64_t last = kNow;
	for (int polite = 0; polite < 99; ++polite) {
		const uint64_t now = last + MIN_REQUESTTIME;
		score = AggressiveScoreAfter(score, now, last);
		last = now;
		if (FitsRequestBudget(score, last + 1000, last)) {
			score = AggressiveScoreAfter(score, last + 1000, last);
			last += 1000;
			++earlyAsks;
		}
		ASSERT_TRUE(score <= REQUEST_SCORE_BUDGET);
	}
	// After the first two, three polite requests clear each early ask.
	ASSERT_EQUALS(34u, earlyAsks);
}

TEST(RequestPacing, AskTimedBeforeTheAICHRequestIsEarly)
{
	// After a failed rehash, a source due for its ask also gets OP_AICHREQUEST now. An ask timed
	// before that request goes out on the next tick and is scored. Timed after it, it waits.
	const uint64_t aichSent = kNow;
	const uint64_t before = AskAt(PacedLastAskedTime(kNow, kNow - kReask, kReask));
	ASSERT_EQUALS(3u, AggressiveScoreAfter(0, before, aichSent));
	const uint64_t after = AskAt(PacedLastAskedTime(kNow, aichSent, kReask));
	ASSERT_EQUALS(0u, AggressiveScoreAfter(0, after, aichSent));
}

TEST(RequestPacing, UnpacedAICHRequestsReachTheBan)
{
	// Four corrupt parts two minutes apart, each sending OP_AICHREQUEST to the one source that
	// has the AICH hashset, the first a minute after its last ask. aMule bans at 10.
	const uint64_t minute = 60 * 1000;
	unsigned score = 0;
	uint64_t last = kNow - minute;
	uint64_t now = kNow;
	for (const unsigned expected : { 3u, 6u, 9u, 12u }) {
		score = AggressiveScoreAfter(score, now, last);
		ASSERT_EQUALS(expected, score);
		last = now;
		now += 2 * minute;
	}
}

TEST(RequestPacing, BudgetAllowsTwoEarlyRequests)
{
	const uint64_t recent = kNow - MIN_REQUESTTIME + 1;
	ASSERT_TRUE(FitsRequestBudget(3, kNow, recent));
	ASSERT_FALSE(FitsRequestBudget(4, kNow, recent));
	static_assert(REQUEST_SCORE_BUDGET + EARLY_REQUEST_SCORE < 10,
		"one early request we did not count must stay below aMule's ban at 10");
	ASSERT_TRUE(FitsRequestBudget(0, kNow, 0)); // no earlier request
	// A polite request lowers the score, so it fits even above the budget.
	ASSERT_TRUE(FitsRequestBudget(9, kNow, kNow - MIN_REQUESTTIME));
}

TEST(RequestPacing, AICHRequestAndAskGoAtOnceAfterAFailedRehash)
{
	// The only AICH source was asked 5 minutes before the rehash failed. The AICH request and the
	// early ask after it both go at once. A third early request waits until MIN_REQUESTTIME
	// after the ask.
	const uint64_t lastAsk = kNow - 5 * 60 * 1000;
	ASSERT_TRUE(FitsRequestBudget(0, kNow, lastAsk));
	unsigned score = AggressiveScoreAfter(0, kNow, lastAsk);
	ASSERT_TRUE(FitsRequestBudget(score, kNow + 1000, kNow));
	score = AggressiveScoreAfter(score, kNow + 1000, kNow);
	ASSERT_EQUALS(6u, score);
	ASSERT_FALSE(FitsRequestBudget(score, kNow + 2000, kNow + 1000));
	ASSERT_TRUE(FitsRequestBudget(score, kNow + 1000 + MIN_REQUESTTIME, kNow + 1000));
}

TEST(RequestPacing, BudgetedAICHRequestsStayBelowTheBan)
{
	// The same four parts. A part waits and retries each minute, before the source's regular ask,
	// as in CPartFile::Process(). A regular ask that would not fit waits as well.
	const uint64_t minute = 60 * 1000;
	unsigned score = 0;
	uint64_t last = kNow - minute;
	uint64_t nextAsk = last + kReask;
	unsigned waiting = 0;
	uint64_t firstSent = 0;
	unsigned sent = 0;
	for (uint64_t now = kNow; now < kNow + 3 * 60 * minute; now += minute) {
		if (now < kNow + 8 * minute && (now - kNow) % (2 * minute) == 0) {
			++waiting;
		}
		if (waiting > 0 && FitsRequestBudget(score, now, last)) {
			score = AggressiveScoreAfter(score, now, last);
			last = now;
			firstSent = firstSent ? firstSent : now;
			--waiting;
			++sent;
		}
		if (now >= nextAsk) {
			if (FitsRequestBudget(score, now, last)) {
				score = AggressiveScoreAfter(score, now, last);
				last = now;
				nextAsk = now + kReask;
			} else {
				nextAsk = last + MIN_REQUESTTIME;
			}
		}
		ASSERT_TRUE(score <= REQUEST_SCORE_BUDGET);
	}
	ASSERT_EQUALS(4u, sent);
	ASSERT_EQUALS(kNow, firstSent);
}

TEST(RequestPacing, DeferredReaskKeepsTheTCPAskBehindIt)
{
	// UDPReaskForDownload() defers a reask that would not fit. CPartFile::Process() then sends it
	// once it is polite, and the TCP ask for an unanswered one UDP_REASK_LEAD later.
	const uint64_t last = kNow - 60 * 1000;
	const uint64_t lastAsked = PacedLastAskedTime(kNow, last + UDP_REASK_LEAD, kReask);
	ASSERT_EQUALS(last + MIN_REQUESTTIME, lastAsked + kReask - UDP_REASK_LEAD);
	ASSERT_EQUALS(last + MIN_REQUESTTIME + UDP_REASK_LEAD, AskAt(lastAsked));
}

namespace
{
// A queued source whose OP_REASKFILEPING arrives but is never answered, so CPartFile::Process()
// follows each one with a TCP ask UDP_REASK_LEAD later. Returns the highest score, and the
// longest time between two TCP asks.
unsigned UnansweredReasks(bool budgeted, uint64_t duration, uint64_t &longestGap)
{
	const uint64_t second = 1000;
	unsigned score = 0;
	unsigned highest = 0;
	uint64_t last = kNow - 60 * second;
	uint64_t lastAsked = last;
	uint64_t lastTCPAsk = last;
	bool udpPending = false;
	longestGap = 0;
	for (uint64_t now = kNow; now < kNow + duration; now += second) {
		using SourceRetryPolicy::IsDue;
		if (!udpPending && IsDue(now, lastAsked, kReask - UDP_REASK_LEAD)) {
			if (!budgeted || FitsRequestBudget(score, now, last)) {
				score = AggressiveScoreAfter(score, now, last);
				last = now;
				udpPending = true;
			} else {
				lastAsked = PacedLastAskedTime(now, last + UDP_REASK_LEAD, kReask);
			}
		}
		if (IsDue(now, lastAsked, kReask)) {
			if (!budgeted || FitsRequestBudget(score, now, last)) {
				score = AggressiveScoreAfter(score, now, last);
				last = now;
				lastAsked = now;
				udpPending = false;
				longestGap = std::max(longestGap, now - lastTCPAsk);
				lastTCPAsk = now;
			} else {
				lastAsked = PacedLastAskedTime(now, last, kReask);
			}
		}
		highest = std::max(highest, score);
	}
	return highest;
}
} // namespace

TEST(RequestPacing, UnansweredReasksReachTheBan)
{
	uint64_t longestGap = 0;
	ASSERT_TRUE(UnansweredReasks(false, 6 * kReask, longestGap) >= 10);
}

TEST(RequestPacing, BudgetedUnansweredReasksStayBelowTheBan)
{
	uint64_t longestGap = 0;
	ASSERT_TRUE(UnansweredReasks(true, 24 * 60 * 60 * 1000, longestGap) <= REQUEST_SCORE_BUDGET);
	// A TCP ask waits at most until the reask before it is no longer early.
	ASSERT_TRUE(longestGap <= kReask + MIN_REQUESTTIME);
}

TEST(RequestPacing, MemoryKeepsOnlyWhatAFreshClientWouldMiss)
{
	CRequestScoreMemory<int> memory;
	memory.Remember(1, 3, kNow - 1000, kNow);
	memory.Remember(2, 0, kNow - 1000, kNow);            // the next request would be early
	memory.Remember(3, 0, kNow - MIN_REQUESTTIME, kNow); // a fresh client scores it the same
	memory.Remember(4, 2, 0, kNow);                      // no request
	ASSERT_EQUALS(2u, memory.Size());
	unsigned score = 0;
	uint64_t last = 0;
	memory.Recall(1, kNow, score, last);
	ASSERT_EQUALS(3u, score);
	ASSERT_EQUALS(kNow - 1000, last);
	ASSERT_EQUALS(1u, memory.Size());
}

TEST(RequestPacing, MemoryMergesAndExpires)
{
	CRequestScoreMemory<int> memory;
	memory.Remember(1, 3, kNow - 1000, kNow);
	unsigned score = 5;
	uint64_t last = kNow - 5000;
	memory.Recall(1, kNow, score, last);
	ASSERT_EQUALS(5u, score);
	ASSERT_EQUALS(kNow - 1000, last);

	memory.Remember(2, 3, kNow, kNow);
	score = 0;
	last = 0;
	memory.Recall(2, kNow + REQUEST_SCORE_MEMORY, score, last);
	ASSERT_EQUALS(0u, score);
	ASSERT_EQUALS(0u, memory.Size());

	memory.Remember(3, 3, kNow, kNow);
	memory.Prune(kNow + REQUEST_SCORE_MEMORY - 1);
	ASSERT_EQUALS(1u, memory.Size());
	memory.Prune(kNow + REQUEST_SCORE_MEMORY);
	ASSERT_EQUALS(0u, memory.Size());
}
