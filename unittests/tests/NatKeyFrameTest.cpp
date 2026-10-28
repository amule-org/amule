//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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

#include "NatKeyFrame.h"

using namespace muleunit;
using namespace NatKeyFrame;

DECLARE_SIMPLE(NatKeyFrame)

namespace
{
Hash MakeHash(std::uint8_t seed)
{
	Hash hash{};
	for (std::size_t i = 0; i < hash.size(); ++i) {
		hash[i] = static_cast<std::uint8_t>(seed + i);
	}
	return hash;
}
} // namespace

TEST(NatKeyFrame, BuildsTheSixteenHashBytesAndNothingElse)
{
	const Hash hash = MakeHash(1);
	const auto payload = BuildPayload(hash);
	ASSERT_EQUALS(16, static_cast<int>(payload.size()));
	ASSERT_TRUE(std::equal(payload.begin(), payload.end(), hash.begin()));
}

TEST(NatKeyFrame, ParsesAnExactFrame)
{
	const Hash hash = MakeHash(7);
	const auto parsed = ParsePayload(hash.data(), hash.size());
	ASSERT_TRUE(parsed.has_value());
	ASSERT_TRUE(*parsed == hash);
}

TEST(NatKeyFrame, ShorterThanAHashIsRejected)
{
	const Hash hash = MakeHash(7);
	ASSERT_FALSE(ParsePayload(hash.data(), hash.size() - 1).has_value());
	ASSERT_FALSE(ParsePayload(hash.data(), 0).has_value());
	ASSERT_FALSE(ParsePayload(nullptr, 16).has_value());
}

TEST(NatKeyFrame, TrailingBytesAreIgnoredLikeEMuleAI)
{
	std::array<std::uint8_t, 20> padded{};
	const Hash hash = MakeHash(3);
	std::copy(hash.begin(), hash.end(), padded.begin());
	const auto parsed = ParsePayload(padded.data(), padded.size());
	ASSERT_TRUE(parsed.has_value());
	ASSERT_TRUE(*parsed == hash);
}

TEST(NatKeyFrame, AnAllZeroHashIsNotAKey)
{
	const Hash zero{};
	ASSERT_FALSE(ParsePayload(zero.data(), zero.size()).has_value());
}

TEST(NatKeyFrame, LearnedHashIsReturnedForItsEndpointOnly)
{
	CPeerKeyCache cache(8, 1000);
	cache.Learn(0x01020304, 4672, MakeHash(1), 0);
	ASSERT_TRUE(cache.Lookup(0x01020304, 4672, 1).has_value());
	ASSERT_FALSE(cache.Lookup(0x01020304, 4673, 1).has_value());
	ASSERT_FALSE(cache.Lookup(0x01020305, 4672, 1).has_value());
}

TEST(NatKeyFrame, LearnedHashExpiresWhenIdle)
{
	CPeerKeyCache cache(8, 1000);
	cache.Learn(1, 1, MakeHash(1), 0);
	ASSERT_TRUE(cache.Lookup(1, 1, 999).has_value());
	ASSERT_FALSE(cache.Lookup(1, 1, 2000).has_value());
}

TEST(NatKeyFrame, UseKeepsAnActiveEndpointAlive)
{
	CPeerKeyCache cache(8, 1000);
	cache.Learn(1, 1, MakeHash(1), 0);
	ASSERT_TRUE(cache.Lookup(1, 1, 900).has_value());
	ASSERT_TRUE(cache.Lookup(1, 1, 1800).has_value());
}

TEST(NatKeyFrame, ANewKeyReplacesTheOldOneForTheSameEndpoint)
{
	CPeerKeyCache cache(8, 1000);
	cache.Learn(1, 1, MakeHash(1), 0);
	cache.Learn(1, 1, MakeHash(9), 10);
	const auto found = cache.Lookup(1, 1, 20);
	ASSERT_TRUE(found.has_value());
	ASSERT_TRUE(*found == MakeHash(9));
}

TEST(NatKeyFrame, TheCacheNeverGrowsPastItsCapacity)
{
	CPeerKeyCache cache(3, 100000);
	for (std::uint32_t ip = 1; ip <= 10; ++ip) {
		cache.Learn(ip, 1, MakeHash(static_cast<std::uint8_t>(ip)), ip);
	}
	ASSERT_TRUE(cache.Size() <= 3);
	ASSERT_TRUE(cache.Lookup(10, 1, 11).has_value());
}

TEST(NatKeyFrame, AZeroCapacityCacheLearnsNothing)
{
	CPeerKeyCache cache(0, 1000);
	cache.Learn(1, 1, MakeHash(1), 0);
	ASSERT_EQUALS(0, static_cast<int>(cache.Size()));
	ASSERT_FALSE(cache.Lookup(1, 1, 0).has_value());
}

TEST(NatKeyFrame, SendGateAllowsOnePerEndpointPerWindow)
{
	CSendGate gate(8, 10000);
	ASSERT_TRUE(gate.TryAcquire(1, 1, 0));
	ASSERT_FALSE(gate.TryAcquire(1, 1, 9999));
	ASSERT_TRUE(gate.TryAcquire(1, 1, 10000));
}

TEST(NatKeyFrame, SendGateTreatsEndpointsIndependently)
{
	CSendGate gate(8, 10000);
	ASSERT_TRUE(gate.TryAcquire(1, 1, 0));
	ASSERT_TRUE(gate.TryAcquire(1, 2, 0));
	ASSERT_TRUE(gate.TryAcquire(2, 1, 0));
}

TEST(NatKeyFrame, SendGateStaysBoundedUnderManyEndpoints)
{
	CSendGate gate(4, 10000);
	for (std::uint32_t ip = 1; ip <= 100; ++ip) {
		gate.TryAcquire(ip, 1, ip);
	}
	ASSERT_TRUE(gate.Size() <= 4);
}
