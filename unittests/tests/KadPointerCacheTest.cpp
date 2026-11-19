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
#include "KadPointerCache.h"

using namespace muleunit;

DECLARE_SIMPLE(KadPointerCache)

namespace
{
constexpr uint32_t RECORD_IP = 0x0A000001;
constexpr uint32_t DEAD_BUDDY_IP = 0x0B000001;
constexpr uint32_t BUDDY_IP = 0x0C000001;
constexpr uint16_t BUDDY_PORT = 4672;
constexpr uint32_t NOW = 1000000;
constexpr uint32_t MISS = 900;

const CKadPointerCache::Key SOURCE{ { 1, 2, 3 } };

SKadPointerRecord Pointer(uint32_t buddyIP = BUDDY_IP, uint16_t buddyPort = BUDDY_PORT)
{
	SKadPointerRecord record;
	record.type = 3;
	record.sameSource = true;
	record.sourceIP = RECORD_IP;
	record.sourceIPs = 1;
	record.hasBuddyID = true;
	record.buddy.ip = buddyIP;
	record.buddy.port = buddyPort;
	record.buddy.id[0] = 0x42;
	return record;
}

SKadPointerNext NextAfterDeadBuddy(const CKadPointerCache &cache, unsigned next = 0, uint32_t now = NOW)
{
	return cache.Next(SOURCE, next, DEAD_BUDDY_IP, BUDDY_PORT, now);
}
} // namespace

TEST(KadPointerCache, APointerFromTheRecordIPIsAccepted)
{
	ASSERT_TRUE(CheckKadPointerRecord(Pointer(), RECORD_IP) == EKadPointerVerdict::Accepted);
}

// §5.6.1: only a type 3 record under the source ID, with a buddy to ask.
TEST(KadPointerCache, AnyOtherRecordIsRefused)
{
	SKadPointerRecord record = Pointer();
	record.type = 5;
	ASSERT_TRUE(CheckKadPointerRecord(record, RECORD_IP) == EKadPointerVerdict::NotTypeThree);
	record = Pointer();
	record.sameSource = false;
	ASSERT_TRUE(CheckKadPointerRecord(record, RECORD_IP) == EKadPointerVerdict::OtherSource);
	record = Pointer();
	record.hasBuddyID = false;
	ASSERT_TRUE(CheckKadPointerRecord(record, RECORD_IP) == EKadPointerVerdict::NoBuddyRoute);
	ASSERT_TRUE(CheckKadPointerRecord(Pointer(0), RECORD_IP) == EKadPointerVerdict::NoBuddyRoute);
	ASSERT_TRUE(
		CheckKadPointerRecord(Pointer(BUDDY_IP, 0), RECORD_IP) == EKadPointerVerdict::NoBuddyRoute);
}

// §5.6.2: the storer recorded the IP the pointer came from; it must be the record's.
TEST(KadPointerCache, APointerFromAnotherIPIsRefused)
{
	ASSERT_TRUE(CheckKadPointerRecord(Pointer(), RECORD_IP + 1) == EKadPointerVerdict::OtherSourceIP);
	SKadPointerRecord record = Pointer();
	record.sourceIPs = 2;
	ASSERT_TRUE(CheckKadPointerRecord(record, RECORD_IP) == EKadPointerVerdict::OtherSourceIP);
	record.sourceIPs = 0;
	ASSERT_TRUE(CheckKadPointerRecord(record, RECORD_IP) == EKadPointerVerdict::OtherSourceIP);
}

TEST(KadPointerCache, AnUnknownSourceIsLookedUpAndThenWaits)
{
	CKadPointerCache cache;
	ASSERT_TRUE(NextAfterDeadBuddy(cache).step == EKadPointerStep::Look);
	cache.Started(SOURCE, RECORD_IP);
	ASSERT_TRUE(NextAfterDeadBuddy(cache).step == EKadPointerStep::Wait);
}

// The source does not wait for the lookup to end once it has a buddy to ask.
TEST(KadPointerCache, TheFirstBuddyIsUsedWhileTheLookupRuns)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	ASSERT_TRUE(cache.Add(SOURCE, Pointer()) == EKadPointerVerdict::Accepted);
	const SKadPointerNext next = NextAfterDeadBuddy(cache);
	ASSERT_TRUE(next.step == EKadPointerStep::Use);
	ASSERT_EQUALS(BUDDY_IP, next.buddy.ip);
	ASSERT_EQUALS(BUDDY_PORT, next.buddy.port);
	ASSERT_EQUALS(0x42, next.buddy.id[0]);
	ASSERT_EQUALS(1u, next.next);
	ASSERT_TRUE(NextAfterDeadBuddy(cache, next.next).step == EKadPointerStep::Wait);
}

// Storers that missed a republish hold older pointers: each buddy is listed once, three at most.
TEST(KadPointerCache, EachBuddyIsListedOnceAndThreeAtMost)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	for (uint32_t i = 0; i < 5; ++i) {
		cache.Add(SOURCE, Pointer(BUDDY_IP + i));
		cache.Add(SOURCE, Pointer(BUDDY_IP + i));
	}
	const CKadPointerCache::SOutcome outcome = cache.Finished(SOURCE, NOW, MISS);
	ASSERT_TRUE(outcome.found);
	ASSERT_EQUALS(3u, outcome.buddies);
	ASSERT_EQUALS(10u, outcome.accepted);
	unsigned next = 0;
	for (uint32_t i = 0; i < 3; ++i) {
		const SKadPointerNext step = NextAfterDeadBuddy(cache, next);
		ASSERT_TRUE(step.step == EKadPointerStep::Use);
		ASSERT_EQUALS(BUDDY_IP + i, step.buddy.ip);
		next = step.next;
	}
	ASSERT_TRUE(NextAfterDeadBuddy(cache, next).step == EKadPointerStep::Exhausted);
}

// A pointer naming the buddy that just failed gives nothing new.
TEST(KadPointerCache, TheBuddyThatFailedIsSkipped)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	cache.Add(SOURCE, Pointer(DEAD_BUDDY_IP));
	cache.Finished(SOURCE, NOW, MISS);
	ASSERT_TRUE(NextAfterDeadBuddy(cache).step == EKadPointerStep::Exhausted);
}

TEST(KadPointerCache, RefusedRecordsAreCountedNotListed)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	SKadPointerRecord record = Pointer();
	record.sourceIP = RECORD_IP + 1;
	ASSERT_TRUE(cache.Add(SOURCE, record) == EKadPointerVerdict::OtherSourceIP);
	const CKadPointerCache::SOutcome outcome = cache.Finished(SOURCE, NOW, MISS);
	ASSERT_FALSE(outcome.found);
	ASSERT_EQUALS(1u, outcome.refused);
}

// A record for no running lookup, late or unasked for, is not taken.
TEST(KadPointerCache, ARecordWithoutALookupIsNotTaken)
{
	CKadPointerCache cache;
	ASSERT_TRUE(cache.Add(SOURCE, Pointer()) == EKadPointerVerdict::OtherSource);
	cache.Started(SOURCE, RECORD_IP);
	cache.Finished(SOURCE, NOW, MISS);
	cache.Add(SOURCE, Pointer());
	ASSERT_TRUE(NextAfterDeadBuddy(cache).step == EKadPointerStep::Exhausted);
}

// A miss is not looked up again before the source's next reask; then it is.
TEST(KadPointerCache, AMissLastsTheReaskInterval)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	cache.Finished(SOURCE, NOW, MISS);
	ASSERT_TRUE(NextAfterDeadBuddy(cache, 0, NOW + MISS - 1).step == EKadPointerStep::Exhausted);
	ASSERT_TRUE(NextAfterDeadBuddy(cache, 0, NOW + MISS).step == EKadPointerStep::Look);
}

TEST(KadPointerCache, AHitLastsAnHour)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	cache.Add(SOURCE, Pointer());
	cache.Finished(SOURCE, NOW, MISS);
	ASSERT_TRUE(
		NextAfterDeadBuddy(cache, 0, NOW + KAD_POINTER_HIT_SECONDS - 1).step == EKadPointerStep::Use);
	ASSERT_TRUE(
		NextAfterDeadBuddy(cache, 0, NOW + KAD_POINTER_HIT_SECONDS).step == EKadPointerStep::Look);
}

// Expired during a round, the pointer gives the source nothing more until its next round.
TEST(KadPointerCache, AnExpiredEntryEndsTheRound)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	cache.Add(SOURCE, Pointer());
	cache.Finished(SOURCE, NOW, MISS);
	ASSERT_TRUE(NextAfterDeadBuddy(cache, 1, NOW + KAD_POINTER_HIT_SECONDS).step ==
		    EKadPointerStep::Exhausted);
}

TEST(KadPointerCache, PruneDropsOnlyWhatExpired)
{
	CKadPointerCache cache;
	cache.Started(SOURCE, RECORD_IP);
	cache.Finished(SOURCE, NOW, MISS);
	CKadPointerCache::Key other = SOURCE;
	other[0] = 9;
	cache.Started(other, RECORD_IP);
	cache.Prune(NOW + MISS);
	ASSERT_EQUALS(1u, cache.Size());
	ASSERT_TRUE(
		cache.Next(other, 0, DEAD_BUDDY_IP, BUDDY_PORT, NOW + MISS).step == EKadPointerStep::Wait);
}
