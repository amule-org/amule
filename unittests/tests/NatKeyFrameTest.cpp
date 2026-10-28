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

#include <string>
#include <vector>

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

TEST(NatKeyFrame, ALongHeaderHasTheHighBitSet)
{
	const std::uint8_t longHeader[] = { 0xC3, 0x00 };
	const std::uint8_t shortHeader[] = { 0x43, 0x00 };
	ASSERT_TRUE(IsQuicLongHeader(longHeader, sizeof(longHeader)));
	ASSERT_FALSE(IsQuicLongHeader(shortHeader, sizeof(shortHeader)));
	ASSERT_FALSE(IsQuicLongHeader(longHeader, 0));
	ASSERT_FALSE(IsQuicLongHeader(nullptr, 1));
}

TEST(NatKeyFrame, ALongHeaderToAKnownPeerCarriesOurKeyFrameAndIsObfuscated)
{
	CPeerKeyCache cache(8, 120000);
	CSendGate gate(8, 10000);
	cache.Learn(1, 1, MakeHash(5), 0);
	const SendPlan plan = PlanDatagram(cache, gate, 1, 1, true, 10);
	ASSERT_TRUE(plan.sendKeyFrame);
	ASSERT_TRUE(plan.key.has_value());
	ASSERT_TRUE(*plan.key == MakeHash(5));
}

TEST(NatKeyFrame, OurKeyFrameIsNeverSentWhereItCouldNotBeObfuscated)
{
	CPeerKeyCache cache(8, 120000);
	CSendGate gate(8, 10000);
	const SendPlan plan = PlanDatagram(cache, gate, 1, 1, true, 0);
	ASSERT_FALSE(plan.sendKeyFrame);
	ASSERT_FALSE(plan.key.has_value());
}

TEST(NatKeyFrame, AShortHeaderNeverCarriesAKeyFrameButStaysObfuscated)
{
	CPeerKeyCache cache(8, 120000);
	CSendGate gate(8, 10000);
	cache.Learn(1, 1, MakeHash(5), 0);
	const SendPlan plan = PlanDatagram(cache, gate, 1, 1, false, 10);
	ASSERT_FALSE(plan.sendKeyFrame);
	ASSERT_TRUE(plan.key.has_value());
}

TEST(NatKeyFrame, TheKeyFrameGoesOnceAWindowWhileEveryDatagramStaysObfuscated)
{
	CPeerKeyCache cache(8, 120000);
	CSendGate gate(8, 10000);
	cache.Learn(1, 1, MakeHash(5), 0);
	ASSERT_TRUE(PlanDatagram(cache, gate, 1, 1, true, 10).sendKeyFrame);
	const SendPlan second = PlanDatagram(cache, gate, 1, 1, true, 20);
	ASSERT_FALSE(second.sendKeyFrame);
	ASSERT_TRUE(second.key.has_value());
	ASSERT_TRUE(PlanDatagram(cache, gate, 1, 1, true, 10010).sendKeyFrame);
}

TEST(NatKeyFrame, AnAnnouncedKeyIsStoredForAnUnknownEndpoint)
{
	CPeerKeyCache cache(8, 120000);
	ASSERT_TRUE(cache.LearnAnnounced(1, 1, MakeHash(5), 0));
	ASSERT_TRUE(*cache.Lookup(1, 1, 1) == MakeHash(5));
}

TEST(NatKeyFrame, AnAnnouncedKeyReplacesAnEarlierAnnouncedOne)
{
	CPeerKeyCache cache(8, 120000);
	cache.LearnAnnounced(1, 1, MakeHash(5), 0);
	ASSERT_TRUE(cache.LearnAnnounced(1, 1, MakeHash(9), 1));
	ASSERT_TRUE(*cache.Lookup(1, 1, 2) == MakeHash(9));
}

TEST(NatKeyFrame, AnAnnouncedKeyNeverReplacesTheIdentityWeDialled)
{
	CPeerKeyCache cache(8, 120000);
	cache.Learn(1, 1, MakeHash(5), 0);
	ASSERT_FALSE(cache.LearnAnnounced(1, 1, MakeHash(9), 1));
	ASSERT_TRUE(*cache.Lookup(1, 1, 2) == MakeHash(5));
}

TEST(NatKeyFrame, ADialledIdentityThatExpiredNoLongerProtectsTheEndpoint)
{
	CPeerKeyCache cache(8, 1000);
	cache.Learn(1, 1, MakeHash(5), 0);
	ASSERT_TRUE(cache.LearnAnnounced(1, 1, MakeHash(9), 5000));
	ASSERT_TRUE(*cache.Lookup(1, 1, 5001) == MakeHash(9));
}

TEST(NatKeyFrame, AZeroCapacityCacheStoresNoAnnouncedKey)
{
	CPeerKeyCache cache(0, 1000);
	ASSERT_FALSE(cache.LearnAnnounced(1, 1, MakeHash(5), 0));
}

TEST(NatKeyFrame, AnExpectedPeerIsRemembered)
{
	CExpectedPeers expected(8, 1000);
	ASSERT_FALSE(expected.IsExpected(1, 1, 0));
	expected.Expect(1, 1, 0);
	ASSERT_TRUE(expected.IsExpected(1, 1, 999));
	ASSERT_FALSE(expected.IsExpected(1, 2, 999));
}

TEST(NatKeyFrame, AnExpectationLapses)
{
	CExpectedPeers expected(8, 1000);
	expected.Expect(1, 1, 0);
	ASSERT_FALSE(expected.IsExpected(1, 1, 1000));
}

TEST(NatKeyFrame, ExpectationsStayBounded)
{
	CExpectedPeers expected(3, 100000);
	for (std::uint32_t ip = 1; ip <= 20; ++ip) {
		expected.Expect(ip, 1, ip);
	}
	ASSERT_TRUE(expected.Size() <= 3);
	ASSERT_TRUE(expected.IsExpected(20, 1, 21));
}

namespace
{
//! A service wired to fakes that record what it would have sent.
struct Rig
{
	bool enabled = true;
	bool obfuscation = true;
	bool acceptKeyFrame = true;
	bool acceptDatagram = true;
	std::vector<std::string> events;
	std::vector<std::optional<Hash>> datagramKeys;
	CKeyFrameService service;

	Rig()
	: service(MakeCallbacks())
	{
	}

	Callbacks MakeCallbacks()
	{
		Callbacks callbacks;
		callbacks.enabled = [this] { return enabled; };
		callbacks.obfuscationOn = [this] { return obfuscation; };
		callbacks.sendKeyFrame = [this](std::uint32_t, std::uint16_t, const Hash &) {
			events.push_back("keyframe");
			return acceptKeyFrame;
		};
		callbacks.sendDatagram = [this](const std::uint8_t *,
						 std::size_t,
						 std::uint32_t,
						 std::uint16_t,
						 const Hash *key) {
			events.push_back("datagram");
			datagramKeys.push_back(key != nullptr ? std::optional<Hash>(*key) : std::nullopt);
			return acceptDatagram;
		};
		return callbacks;
	}
};

const std::uint8_t kLongHeader[] = { 0xC3, 0, 0, 0 };
const std::uint8_t kShortHeader[] = { 0x43, 0, 0, 0 };
} // namespace

TEST(NatKeyFrame, AKeyFrameIsIgnoredWhileQuicNatTIsOff)
{
	Rig rig;
	rig.enabled = false;
	rig.service.ExpectPeer(1, 1, 0);
	const Hash hash = MakeHash(5);
	ASSERT_TRUE(rig.service.OnKeyFrame(hash.data(), hash.size(), 1, 1, 1) == KeyFrameResult::Disabled);
	rig.enabled = true;
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, 2);
	ASSERT_FALSE(rig.datagramKeys.back().has_value());
}

TEST(NatKeyFrame, AKeyFrameFromAnUnexpectedEndpointIsIgnored)
{
	Rig rig;
	const Hash hash = MakeHash(5);
	ASSERT_TRUE(rig.service.OnKeyFrame(hash.data(), hash.size(), 1, 1, 0) == KeyFrameResult::Unexpected);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, 1);
	ASSERT_FALSE(rig.datagramKeys.back().has_value());
}

TEST(NatKeyFrame, FailedLongHeaderEnqueueReleasesKeyFrameGate)
{
	Rig rig;
	const Hash hash = MakeHash(5);
	rig.service.NoteDialled(1, 1, hash, 0);
	rig.acceptDatagram = false;
	ASSERT_FALSE(rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 1));
	ASSERT_EQUALS(2, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[0] == "keyframe");
	ASSERT_TRUE(rig.events[1] == "datagram");

	rig.acceptDatagram = true;
	ASSERT_TRUE(rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 2));
	ASSERT_EQUALS(4, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[2] == "keyframe");
	ASSERT_TRUE(rig.events[3] == "datagram");
}

TEST(NatKeyFrame, RejectedKeyFrameRefusesDatagramAndRetriesOnNextSend)
{
	Rig rig;
	const Hash hash = MakeHash(5);
	rig.service.NoteDialled(1, 1, hash, 0);
	rig.acceptKeyFrame = false;
	ASSERT_FALSE(rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 1));
	ASSERT_EQUALS(1, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[0] == "keyframe");

	rig.acceptKeyFrame = true;
	ASSERT_TRUE(rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 2));
	ASSERT_EQUALS(3, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[1] == "keyframe");
	ASSERT_TRUE(rig.events[2] == "datagram");
}

TEST(NatKeyFrame, AKeyFrameFromAnExpectedEndpointObfuscatesOurReplies)
{
	Rig rig;
	rig.service.ExpectPeer(1, 1, 0);
	const Hash hash = MakeHash(5);
	ASSERT_TRUE(rig.service.OnKeyFrame(hash.data(), hash.size(), 1, 1, 1) == KeyFrameResult::Learned);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, 2);
	ASSERT_TRUE(*rig.datagramKeys.back() == hash);
}

TEST(NatKeyFrame, VouchedRendezvousHashSurvivesConflictingDialledAndAnnouncedHashes)
{
	Rig rig;
	const Hash vouched = MakeHash(5);
	const Hash conflicting = MakeHash(9);
	rig.service.ExpectPeer(1, 1, 0, vouched);
	rig.service.NoteDialled(1, 1, conflicting, 1);
	ASSERT_TRUE(rig.service.OnKeyFrame(conflicting.data(), conflicting.size(), 1, 1, 2) ==
		    KeyFrameResult::Held);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, 2);
	ASSERT_TRUE(*rig.datagramKeys.back() == vouched);
}

TEST(NatKeyFrame, ConflictingLiveVouchersFailClosedUntilTheExpectationExpires)
{
	Rig rig;
	const Hash first = MakeHash(5);
	const Hash conflicting = MakeHash(9);
	rig.service.ExpectPeer(1, 1, 0, first);
	rig.service.ExpectPeer(1, 1, 1, conflicting);
	rig.service.ExpectPeer(1, 1, 2); // A weaker duplicate cannot clear the collision.
	rig.service.NoteDialled(1, 1, conflicting, 3);
	ASSERT_TRUE(rig.service.OnKeyFrame(conflicting.data(), conflicting.size(), 1, 1, 4) ==
		    KeyFrameResult::Held);
	ASSERT_FALSE(rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 5));
	ASSERT_EQUALS(0, static_cast<int>(rig.events.size()));
	ASSERT_EQUALS(0, static_cast<int>(rig.datagramKeys.size()));

	// A new expectation after the 30 s session window establishes a fresh authoritative hash.
	const std::uint64_t newSession = kExpectedPeerTtlMs + 3;
	rig.service.ExpectPeer(1, 1, newSession, conflicting);
	ASSERT_TRUE(rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, newSession + 1));
	ASSERT_EQUALS(2, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[0] == "keyframe");
	ASSERT_TRUE(rig.events[1] == "datagram");
	ASSERT_TRUE(*rig.datagramKeys.back() == conflicting);
}

TEST(NatKeyFrame, RepeatedMatchingVouchedHashDoesNotCreateACollision)
{
	Rig rig;
	const Hash vouched = MakeHash(5);
	rig.service.ExpectPeer(1, 1, 0, vouched);
	rig.service.ExpectPeer(1, 1, 1, vouched);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, 2);
	ASSERT_TRUE(*rig.datagramKeys.back() == vouched);
}

TEST(NatKeyFrame, VouchedHashLifetimeTracksActiveUseAndExpiresAfterIdleWindow)
{
	Rig rig;
	const Hash vouched = MakeHash(5);
	const Hash redialled = MakeHash(9);
	rig.service.ExpectPeer(1, 1, 0, vouched);
	// The endpoint expectation expires at 30 s, but active-use key state has a 120 s idle TTL.
	const std::uint64_t firstUse = kExpectedPeerTtlMs;
	const std::uint64_t secondUse = firstUse + kPeerKeyIdleMs - 1;
	const std::uint64_t afterIdleExpiry = secondUse + kPeerKeyIdleMs;
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, firstUse);
	ASSERT_TRUE(*rig.datagramKeys.back() == vouched);
	rig.service.NoteDialled(1, 1, redialled, kPeerKeyIdleMs);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, secondUse);
	ASSERT_TRUE(*rig.datagramKeys.back() == vouched);
	// No use for 120 s expires the vouched entry; a new dial may now establish the key.
	rig.service.NoteDialled(1, 1, redialled, afterIdleExpiry);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, afterIdleExpiry + 1);
	ASSERT_TRUE(*rig.datagramKeys.back() == redialled);
}

TEST(NatKeyFrame, AMalformedKeyFrameLearnsNothing)
{
	Rig rig;
	rig.service.ExpectPeer(1, 1, 0);
	const std::uint8_t shortPayload[8] = {};
	ASSERT_TRUE(rig.service.OnKeyFrame(shortPayload, sizeof(shortPayload), 1, 1, 1) ==
		    KeyFrameResult::Malformed);
}

TEST(NatKeyFrame, TheIdentityWeDialledSurvivesAKeyFrameFromTheSameEndpoint)
{
	Rig rig;
	rig.service.ExpectPeer(1, 1, 0);
	rig.service.NoteDialled(1, 1, MakeHash(5), 0);
	const Hash other = MakeHash(9);
	ASSERT_TRUE(rig.service.OnKeyFrame(other.data(), other.size(), 1, 1, 1) == KeyFrameResult::Held);
	rig.service.SendDatagram(kShortHeader, sizeof(kShortHeader), 1, 1, 2);
	ASSERT_TRUE(*rig.datagramKeys.back() == MakeHash(5));
}

TEST(NatKeyFrame, OurKeyFrameGoesBeforeALongHeaderAndOnlyOncePerWindow)
{
	Rig rig;
	rig.service.NoteDialled(1, 1, MakeHash(5), 0);
	rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 10);
	rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 20);
	ASSERT_EQUALS(3, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[0] == "keyframe");
	ASSERT_TRUE(rig.events[1] == "datagram");
	ASSERT_TRUE(rig.events[2] == "datagram");
}

TEST(NatKeyFrame, NothingIsObfuscatedAndNoKeyFrameSentWhenTheUserTurnedObfuscationOff)
{
	Rig rig;
	rig.obfuscation = false;
	rig.service.NoteDialled(1, 1, MakeHash(5), 0);
	rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 10);
	ASSERT_EQUALS(1, static_cast<int>(rig.events.size()));
	ASSERT_TRUE(rig.events[0] == "datagram");
	ASSERT_FALSE(rig.datagramKeys.back().has_value());
}

TEST(NatKeyFrame, ADatagramToAnUnknownPeerGoesPlainWithoutOurKeyFrame)
{
	Rig rig;
	rig.service.SendDatagram(kLongHeader, sizeof(kLongHeader), 1, 1, 10);
	ASSERT_EQUALS(1, static_cast<int>(rig.events.size()));
	ASSERT_FALSE(rig.datagramKeys.back().has_value());
}
