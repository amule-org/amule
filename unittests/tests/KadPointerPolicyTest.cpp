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
#include "KadPointerPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadPointerPolicy)

namespace
{
constexpr uint32_t BUDDY_IP = 0x0A000001;
constexpr uint16_t BUDDY_PORT = 4672;
constexpr uint32_t LIFETIME = 5 * 3600;
constexpr uint32_t NOW = 1000000;
} // namespace

// The NOT of every byte, so it does not depend on the layout the ID is kept in (§5.6.1).
TEST(KadPointerPolicy, TheKeyIsTheBitwiseNotOfTheSourceID)
{
	uint8_t sourceID[16];
	for (unsigned i = 0; i < 16; ++i) {
		sourceID[i] = static_cast<uint8_t>(i * 17);
	}
	uint8_t key[16];
	KadPointerKey(sourceID, key);
	for (unsigned i = 0; i < 16; ++i) {
		ASSERT_EQUALS(static_cast<uint8_t>(0xFF ^ sourceID[i]), key[i]);
	}
}

// A storer following §5.6 drops the marker and the pointer without our receiver key.
TEST(KadPointerPolicy, OnlyAKeyedStorerGetsThePointer)
{
	ASSERT_TRUE(SendsKadPointer(true, true));
	ASSERT_FALSE(SendsKadPointer(true, false));
	ASSERT_FALSE(SendsKadPointer(false, true));
}

TEST(KadPointerPolicy, ANewBuddyIsPublishedAtOnce)
{
	const SKadPointerState never;
	ASSERT_TRUE(IsKadPointerDue(true, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT, never, NOW));
	const SKadPointerState published = KadPointerStarted({}, BUDDY_IP, BUDDY_PORT, NOW, LIFETIME);
	ASSERT_TRUE(IsKadPointerDue(
		true, EKadSourcePublish::Buddy, BUDDY_IP + 1, BUDDY_PORT, published, NOW + 1));
	ASSERT_TRUE(IsKadPointerDue(
		true, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT + 1, published, NOW + 1));
}

// The same buddy is republished as the record expires on its storers, not sooner.
TEST(KadPointerPolicy, TheSameBuddyIsRepublishedWithTheRecordLifetime)
{
	const SKadPointerState published = KadPointerStarted({}, BUDDY_IP, BUDDY_PORT, NOW, LIFETIME);
	ASSERT_FALSE(IsKadPointerDue(
		true, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT, published, NOW + LIFETIME - 1));
	ASSERT_TRUE(IsKadPointerDue(
		true, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT, published, NOW + LIFETIME));
}

// Without a buddy route there is nothing to point at; the old pointer expires on its storers.
TEST(KadPointerPolicy, NothingGoesOutWithoutABuddyRoute)
{
	const SKadPointerState never;
	ASSERT_FALSE(IsKadPointerDue(true, EKadSourcePublish::None, 0, 0, never, NOW));
	ASSERT_FALSE(IsKadPointerDue(true, EKadSourcePublish::HighID, 0, 0, never, NOW));
	ASSERT_FALSE(IsKadPointerDue(true, EKadSourcePublish::DirectCallback, 0, 0, never, NOW));
	ASSERT_FALSE(IsKadPointerDue(true, EKadSourcePublish::IPv6Only, 0, 0, never, NOW));
	ASSERT_FALSE(IsKadPointerDue(true, EKadSourcePublish::Buddy, 0, 0, never, NOW));
	ASSERT_FALSE(IsKadPointerDue(false, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT, never, NOW));
}

TEST(KadPointerPolicy, AnUnacknowledgedPointerIsTriedAgainSooner)
{
	SKadPointerState state = KadPointerStarted({}, BUDDY_IP, BUDDY_PORT, NOW, LIFETIME);
	KadPointerFinished(state, 0, NOW + 60);
	const uint32_t retry = NOW + 60 + KAD_POINTER_RETRY_SECONDS;
	ASSERT_FALSE(IsKadPointerDue(true, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT, state, retry - 1));
	ASSERT_TRUE(IsKadPointerDue(true, EKadSourcePublish::Buddy, BUDDY_IP, BUDDY_PORT, state, retry));
}

TEST(KadPointerPolicy, AnAcknowledgedPointerKeepsItsLifetime)
{
	SKadPointerState state = KadPointerStarted({}, BUDDY_IP, BUDDY_PORT, NOW, LIFETIME);
	KadPointerFinished(state, 7, NOW + 60);
	ASSERT_EQUALS(NOW + LIFETIME, state.nextDue);
	// A retry never pushes the next publish later than the lifetime would.
	KadPointerFinished(state, 0, NOW + LIFETIME - 1);
	ASSERT_EQUALS(NOW + LIFETIME, state.nextDue);
}

// A republish for the same buddy still has its storers; a new buddy has none until acknowledged.
TEST(KadPointerPolicy, APointerIsLiveForTheBuddyItNamesOnceAcknowledged)
{
	SKadPointerState state = KadPointerStarted({}, BUDDY_IP, BUDDY_PORT, NOW, LIFETIME);
	ASSERT_FALSE(IsKadPointerLive(state, BUDDY_IP, BUDDY_PORT, NOW + 30, LIFETIME));
	KadPointerFinished(state, 2, NOW + 40);
	ASSERT_TRUE(IsKadPointerLive(state, BUDDY_IP, BUDDY_PORT, NOW + 41, LIFETIME));
	ASSERT_FALSE(IsKadPointerLive(state, BUDDY_IP + 1, BUDDY_PORT, NOW + 41, LIFETIME));
	ASSERT_FALSE(IsKadPointerLive(state, BUDDY_IP, BUDDY_PORT, NOW + 40 + LIFETIME, LIFETIME));

	const SKadPointerState again =
		KadPointerStarted(state, BUDDY_IP, BUDDY_PORT, NOW + LIFETIME - 10, LIFETIME);
	ASSERT_TRUE(IsKadPointerLive(again, BUDDY_IP, BUDDY_PORT, NOW + LIFETIME - 5, LIFETIME));
	const SKadPointerState moved = KadPointerStarted(state, BUDDY_IP + 1, BUDDY_PORT, NOW + 60, LIFETIME);
	ASSERT_FALSE(IsKadPointerLive(moved, BUDDY_IP + 1, BUDDY_PORT, NOW + 61, LIFETIME));
}

TEST(KadPointerPolicy, AnUnacknowledgedPointerIsNotLive)
{
	SKadPointerState state = KadPointerStarted({}, BUDDY_IP, BUDDY_PORT, NOW, LIFETIME);
	KadPointerFinished(state, 0, NOW + 40);
	ASSERT_FALSE(IsKadPointerLive(state, BUDDY_IP, BUDDY_PORT, NOW + 41, LIFETIME));
	ASSERT_FALSE(IsKadPointerLive(state, 0, 0, NOW + 41, LIFETIME));
}

// Any marked entry, of type 3, 5 or 7 and whatever buddy it names, while our pointer is live.
TEST(KadPointerPolicy, AFileIsReachableByPointerWithALiveMarkedEntry)
{
	ASSERT_TRUE(KadSourceReachableByPointer(true, NOW - 60, true, NOW, LIFETIME));
	ASSERT_FALSE(KadSourceReachableByPointer(false, NOW - 60, true, NOW, LIFETIME));
	ASSERT_FALSE(KadSourceReachableByPointer(true, NOW - 60, false, NOW, LIFETIME));
	ASSERT_FALSE(KadSourceReachableByPointer(true, NOW - LIFETIME, true, NOW, LIFETIME));
	ASSERT_FALSE(KadSourceReachableByPointer(true, 0, true, NOW, LIFETIME));
}

// With a buddy back, the due files whose entries lead nowhere go first; those the pointer reaches
// wait for them, and only for them.
TEST(KadPointerPolicy, FilesReachableByPointerWaitForTheOthers)
{
	ASSERT_TRUE(DefersKadSourcePublish(true, 4000));
	ASSERT_FALSE(DefersKadSourcePublish(true, 0));
	ASSERT_FALSE(DefersKadSourcePublish(false, 4000));
	ASSERT_FALSE(DefersKadSourcePublish(false, 0));
}
