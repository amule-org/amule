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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include <muleunit/test.h>
#include "GlobalSourceQueue.h"

using namespace muleunit;

DECLARE_SIMPLE(GlobalSourceQueue)

namespace
{
const uint64_t T0 = 5000000;
const uint32_t PER_PACKET = 31;

// Every byte the same, so the hashes sort by v.
CMD4Hash Hash(unsigned char v)
{
	unsigned char raw[16];
	std::fill(raw, raw + 16, v);
	return CMD4Hash(raw);
}

std::vector<CMD4Hash> Hashes(unsigned char first, unsigned char count)
{
	std::vector<CMD4Hash> result;
	for (unsigned char v = first; v < first + count; ++v) {
		result.push_back(Hash(v));
	}
	return result;
}

bool Any(const CMD4Hash &)
{
	return true;
}

const std::vector<CMD4Hash> NONE;
} // namespace

TEST(GlobalSourceQueue, ANewDownloadIsAskedAtOnce)
{
	CGlobalSourceQueue queue;
	queue.Add(Hash(7));
	const std::vector<CMD4Hash> asked = queue.Next(T0, NONE, Any, PER_PACKET);
	ASSERT_EQUALS(1u, static_cast<uint32_t>(asked.size()));
	ASSERT_TRUE(asked[0] == Hash(7));
}

TEST(GlobalSourceQueue, NewDownloadsGoNewestFirst)
{
	CGlobalSourceQueue queue;
	queue.Add(Hash(3));
	queue.Add(Hash(1));
	queue.Add(Hash(2));
	queue.Add(Hash(3));
	const std::vector<CMD4Hash> asked = queue.Next(T0, NONE, Any, PER_PACKET);
	ASSERT_EQUALS(3u, static_cast<uint32_t>(asked.size()));
	ASSERT_TRUE(asked[0] == Hash(3));
	ASSERT_TRUE(asked[1] == Hash(2));
	ASSERT_TRUE(asked[2] == Hash(1));
}

TEST(GlobalSourceQueue, AnUnwantedNewDownloadCostsNothing)
{
	CGlobalSourceQueue queue;
	queue.Add(Hash(1));
	const std::vector<CMD4Hash> asked =
		queue.Next(T0, NONE, [](const CMD4Hash &) { return false; }, PER_PACKET);
	ASSERT_TRUE(asked.empty());
	ASSERT_EQUALS(CGlobalSourceQueue::HASHES, queue.Available(T0));
}

TEST(GlobalSourceQueue, OthersGoInHashOrderFromTheCursor)
{
	CGlobalSourceQueue queue(Hash(2));
	const std::vector<CMD4Hash> asked = queue.Next(T0, Hashes(1, 4), Any, PER_PACKET);
	ASSERT_EQUALS(4u, static_cast<uint32_t>(asked.size()));
	ASSERT_TRUE(asked[0] == Hash(3));
	ASSERT_TRUE(asked[1] == Hash(4));
	ASSERT_TRUE(asked[2] == Hash(1));
	ASSERT_TRUE(asked[3] == Hash(2));
}

TEST(GlobalSourceQueue, OneAskedRecentlyWaitsForTheReaskTime)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> downloads = Hashes(1, 3);
	ASSERT_EQUALS(3u, static_cast<uint32_t>(queue.Next(T0, downloads, Any, PER_PACKET).size()));
	ASSERT_TRUE(queue.Next(T0 + UDPSERVERREASKTIME - 1, downloads, Any, PER_PACKET).empty());
	ASSERT_EQUALS(3u,
		static_cast<uint32_t>(
			queue.Next(T0 + UDPSERVERREASKTIME, downloads, Any, PER_PACKET).size()));
}

TEST(GlobalSourceQueue, TheTurnContinuesWhereItStopped)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> downloads = Hashes(1, 40);
	const std::vector<CMD4Hash> first = queue.Next(T0, downloads, Any, PER_PACKET);
	ASSERT_EQUALS(CGlobalSourceQueue::HASHES - CGlobalSourceQueue::RESERVE,
		static_cast<uint32_t>(first.size()));
	ASSERT_TRUE(first.back() == Hash(31));
	const std::vector<CMD4Hash> next =
		queue.Next(T0 + CGlobalSourceQueue::GAP_MS, downloads, Any, PER_PACKET);
	ASSERT_EQUALS(1u, static_cast<uint32_t>(next.size()));
	ASSERT_TRUE(next[0] == Hash(32));
}

TEST(GlobalSourceQueue, TheReserveIsLeftForNewDownloads)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> downloads = Hashes(1, 100);
	queue.Next(T0, downloads, Any, PER_PACKET);
	ASSERT_EQUALS(CGlobalSourceQueue::RESERVE, queue.Available(T0));
	ASSERT_TRUE(queue.Next(T0 + CGlobalSourceQueue::GAP_MS - 1, downloads, Any, PER_PACKET).empty());

	queue.Add(Hash(200));
	queue.Add(Hash(201));
	const std::vector<CMD4Hash> asked =
		queue.Next(T0 + CGlobalSourceQueue::NEW_GAP_MS, downloads, Any, PER_PACKET);
	ASSERT_EQUALS(2u, static_cast<uint32_t>(asked.size()));
	ASSERT_TRUE(asked[0] == Hash(201));
}

TEST(GlobalSourceQueue, NewDownloadsWaitFifteenSecondsAfterARequest)
{
	CGlobalSourceQueue queue;
	queue.Add(Hash(1));
	queue.Next(T0, NONE, Any, PER_PACKET);
	queue.Add(Hash(2));
	ASSERT_TRUE(queue.Next(T0 + CGlobalSourceQueue::NEW_GAP_MS - 1, NONE, Any, PER_PACKET).empty());
	ASSERT_EQUALS(1u,
		static_cast<uint32_t>(
			queue.Next(T0 + CGlobalSourceQueue::NEW_GAP_MS, NONE, Any, PER_PACKET).size()));
}

TEST(GlobalSourceQueue, OthersWaitAMinuteAfterARequest)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> downloads = Hashes(1, 40);
	queue.Next(T0, downloads, Any, PER_PACKET);
	const uint64_t refilled = T0 + 10 * CGlobalSourceQueue::MS_PER_HASH;
	ASSERT_FALSE(queue.Next(refilled, downloads, Any, PER_PACKET).empty());
	ASSERT_TRUE(
		queue.Next(refilled + CGlobalSourceQueue::GAP_MS - 1, downloads, Any, PER_PACKET).empty());
}

TEST(GlobalSourceQueue, AServerTakingOneHashAPacketStillGetsItsTurn)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> asked = queue.Next(T0, Hashes(1, 5), Any, 1);
	ASSERT_EQUALS(1u, static_cast<uint32_t>(asked.size()));
}

TEST(GlobalSourceQueue, AnHourOfAddsStaysWithinTheBudget)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> downloads = Hashes(1, 200);
	size_t total = 0;
	unsigned char next = 210;
	for (uint64_t ms = 0; ms < 3600000; ms += 1000) {
		if (ms % 10000 == 0 && next < 255) {
			queue.Add(Hash(next++));
		}
		total += queue.Next(T0 + ms, downloads, Any, PER_PACKET).size();
	}
	ASSERT_TRUE(total <= CGlobalSourceQueue::HASHES + 3600000 / CGlobalSourceQueue::MS_PER_HASH);
	ASSERT_TRUE(total >= 3600000 / CGlobalSourceQueue::MS_PER_HASH);
}

TEST(GlobalSourceQueue, NewDownloadsGetAShortBurstThenOneRequestAMinute)
{
	CGlobalSourceQueue queue;
	unsigned char next = 1;
	uint32_t packets = 0;
	uint32_t late = 0;
	for (uint64_t ms = 0; ms < 600000; ms += CGlobalSourceQueue::NEW_GAP_MS) {
		queue.Add(Hash(next++));
		if (!queue.Next(T0 + ms, NONE, Any, PER_PACKET).empty()) {
			++packets;
			late += (ms >= 300000);
		}
	}
	ASSERT_TRUE(packets <= CGlobalSourceQueue::PACKETS + 600000 / CGlobalSourceQueue::MS_PER_PACKET + 1);
	ASSERT_TRUE(late <= 300000 / CGlobalSourceQueue::MS_PER_PACKET + 1);
}

TEST(GlobalSourceQueue, OthersLeaveTheLastPacketForNewDownloads)
{
	CGlobalSourceQueue queue;
	const std::vector<CMD4Hash> downloads = Hashes(1, 100);
	uint64_t ms = 0;
	for (unsigned char v = 200; v < 206; ++v, ms += CGlobalSourceQueue::NEW_GAP_MS) {
		queue.Add(Hash(v));
		queue.Next(T0 + ms, NONE, Any, PER_PACKET);
	}
	const uint64_t afterGap = T0 + ms - CGlobalSourceQueue::NEW_GAP_MS + CGlobalSourceQueue::GAP_MS;
	ASSERT_EQUALS(CGlobalSourceQueue::PACKET_RESERVE, queue.PacketsAvailable(afterGap));
	ASSERT_TRUE(queue.Next(afterGap, downloads, Any, PER_PACKET).empty());
	queue.Add(Hash(250));
	ASSERT_FALSE(queue.Next(afterGap, downloads, Any, PER_PACKET).empty());
}
