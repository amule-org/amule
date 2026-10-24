// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "UploadRefill.h"
#include "UploadBuffering.h"

using namespace muleunit;
DECLARE_SIMPLE(UploadRefill)

namespace
{
constexpr uint32_t block = 184320;
}

TEST(UploadRefill, SlowSlotRefillsWithABlockInReserve)
{
	UploadRefill refill;
	refill.SetTarget(block + 1);
	refill.Queue(2 * block);
	ASSERT_FALSE(refill.Complete(10240, 0));
	ASSERT_TRUE(refill.Complete(block - 10240, 0));
	// A delayed read must not cause a wake-up on every following packet.
	ASSERT_FALSE(refill.Complete(10240, 0));
	refill.Queue(block);
	ASSERT_TRUE(refill.Complete(block - 10240, 0));
}

TEST(UploadRefill, FastSlotKeepsItsSendAheadReserve)
{
	UploadRefill refill;
	refill.SetTarget(10 * block + 1);
	refill.Queue(11 * block);
	ASSERT_FALSE(refill.Complete(block - 1, 0));
	ASSERT_TRUE(refill.Complete(1, 0));
	refill.Queue(block);
	ASSERT_TRUE(refill.Complete(block, 0));
}

TEST(UploadRefill, ShortRequestAndCompressedPacketDrainWakeTheWorker)
{
	UploadRefill refill;
	refill.SetTarget(block + 1);
	refill.Queue(4096);
	ASSERT_TRUE(refill.Complete(4096, 0));
	// Completion uses original payload, irrespective of compressed wire length.
	refill.Queue(2 * block);
	ASSERT_TRUE(refill.Complete(block, 0));
	ASSERT_TRUE(refill.Complete(block, 0));
}

TEST(UploadRefill, NoPayloadAndInactiveSocketsDoNotWake)
{
	UploadRefill refill;
	refill.Queue(block);
	ASSERT_FALSE(refill.Complete(block, 0));
	refill.SetTarget(block + 1);
	ASSERT_FALSE(refill.Complete(0, 0));
}

TEST(UploadRefill, DisconnectClearsOldPayloadAndTarget)
{
	UploadRefill refill;
	refill.SetTarget(block + 1);
	refill.Queue(2 * block);
	refill.Clear();
	refill.Queue(4096);
	ASSERT_FALSE(refill.Complete(4096, 0));
	refill.SetTarget(block + 1);
	refill.Queue(4096);
	ASSERT_TRUE(refill.Complete(4096, 0));
}

TEST(UploadRefill, TruncationKeepsTheCurrentPacketInTheCount)
{
	UploadRefill refill;
	refill.SetTarget(block + 1);
	refill.Queue(2 * block);
	refill.Discard(block);
	ASSERT_TRUE(refill.Complete(block, 0));
}

TEST(UploadRefill, RaisingTargetAfterDrainRequestsAnotherWorkerPass)
{
	UploadRefill refill;
	refill.SetTarget(block + 1);
	refill.Queue(11 * block);
	// The worker's fast-slot snapshot still sees eleven blocks. A completion then
	// drains two blocks while the old slow-slot target is installed: no crossing.
	ASSERT_FALSE(refill.Complete(2 * block, 0));
	ASSERT_TRUE(refill.SetTarget(10 * block + 1));
	// Coalesce repeated checks while the refill is in progress.
	ASSERT_FALSE(refill.SetTarget(10 * block + 1));
	refill.Queue(2 * block);
	ASSERT_TRUE(refill.Complete(block, 0));
}

TEST(UploadRefill, RaisingTargetWithEnoughQueuedPayloadWaitsForCrossing)
{
	UploadRefill refill;
	refill.SetTarget(block + 1);
	refill.Queue(11 * block);
	ASSERT_FALSE(refill.SetTarget(10 * block + 1));
	ASSERT_TRUE(refill.Complete(block, 0));
}

TEST(UploadRefill, LoweringTargetDoesNotScheduleAnUnneededRefill)
{
	UploadRefill refill;
	refill.SetTarget(10 * block + 1);
	refill.Queue(2 * block);
	ASSERT_FALSE(refill.SetTarget(block + 1));
	ASSERT_TRUE(refill.Complete(block, 0));
}

TEST(UploadRefill, OriginalPayloadDrainSizesCompressedUploadsAndExpires)
{
	UploadRefill refill;
	refill.Queue(3 * block);
	ASSERT_FALSE(refill.Complete(3 * block, 100));
	const uint32_t wireRate = 16 * 1024;
	const uint32_t sourceRate = refill.PayloadRate(100);
	ASSERT_EQUALS(3 * block, sourceRate);
	ASSERT_EQUALS(6 * block + 1, UploadBufferTarget(std::max(wireRate, sourceRate), block));
	ASSERT_TRUE(UploadBufferTarget(sourceRate, block) > UploadBufferTarget(wireRate, block));
	ASSERT_EQUALS(sourceRate, refill.PayloadRate(1099));
	ASSERT_EQUALS(uint32_t{ 0 }, refill.PayloadRate(1100));
}

TEST(UploadRefill, RetargetAndDiscardPreserveRateButDisconnectClearsIt)
{
	UploadRefill refill;
	ASSERT_TRUE(refill.SetTarget(block + 1));
	refill.Queue(2 * block);
	ASSERT_TRUE(refill.Complete(block, 100));
	ASSERT_TRUE(refill.SetTarget(10 * block + 1));
	refill.Discard(block);
	ASSERT_EQUALS(uint64_t{ 0 }, refill.Queued());
	ASSERT_EQUALS(block, refill.PayloadRate(200));
	refill.Clear();
	ASSERT_EQUALS(uint32_t{ 0 }, refill.PayloadRate(200));
}
