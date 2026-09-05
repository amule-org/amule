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

// CT_MOD_YOUR_IP (0xAD) carries a peer's opinion of our own public address.
// eMuleAI believes the first peer that offers one; emule-qt does not, and
// neither does this. What is tested here is the "does not": that one voice is
// never enough, that one host cannot be several voices by repeating itself,
// and that the quorum is keyed on where a packet was seen coming from rather
// than on anything the sender chose for itself.
//
// The rule has no runtime signal if it is wrong. A client that accepts a bad
// address for itself does not fail -- it goes on working, publishing an
// address nobody can reach, so the tests have to state the rule rather than
// wait for a symptom.
//
// CUpDownClient reaches theApp and cannot be linked into a unit test, so the
// tracker lives in a header of its own, like CPeerCapabilities.

#include <muleunit/test.h>

#include <PublicIPv6Corroboration.h>

#include <cstring>

using namespace muleunit;

DECLARE_SIMPLE(PublicIPv6Corroboration)

namespace
{

// 2001:db8::<last>, the documentation prefix. The last byte is the only thing
// that varies, so two addresses differ in a single byte -- the case a
// byte-wise comparison is most likely to get wrong.
CPublicIPv6Corroboration::Address MakeAddress(uint8_t last)
{
	CPublicIPv6Corroboration::Address address = {};
	address[0] = 0x20;
	address[1] = 0x01;
	address[2] = 0x0d;
	address[3] = 0xb8;
	address[15] = last;
	return address;
}

// 203.0.113.<host>, host order, as GetConnectIP() would report it.
uint32_t MakeObserver(uint8_t host)
{
	return 0xCB007100u | host;
}

} // namespace

// The threshold is a policy decision, not an implementation detail: raising or
// lowering it changes how much a stranger's word is worth. Pinned as a literal
// so the change has to be deliberate.
TEST(PublicIPv6Corroboration, ThresholdIsThreeDistinctObservers)
{
	ASSERT_EQUALS(3u, (unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD);
}

// The whole point. eMuleAI would have believed this one.
TEST(PublicIPv6Corroboration, OnePeerIsNotBelieved)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address claimed = MakeAddress(0x01);

	ASSERT_FALSE(tracker.AddClaim(MakeObserver(1), claimed.data()));
	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_TRUE(tracker.CorroboratedAddress() == nullptr);
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}

// Two is still one short, and it is the interesting near-miss: two source
// addresses is one dual-homed host as easily as it is two opinions.
TEST(PublicIPv6Corroboration, TwoPeersAreStillNotEnough)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address claimed = MakeAddress(0x01);

	tracker.AddClaim(MakeObserver(1), claimed.data());
	ASSERT_FALSE(tracker.AddClaim(MakeObserver(2), claimed.data()));
	ASSERT_FALSE(tracker.IsCorroborated());
}

// N distinct observed addresses saying the same thing, and the value is
// believed -- and it is the claimed value that comes back out, byte for byte.
TEST(PublicIPv6Corroboration, DistinctObserversReachingTheThresholdCorroborate)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address claimed = MakeAddress(0x01);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD - 1; ++i) {
		ASSERT_FALSE(tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data()));
	}
	ASSERT_TRUE(
		tracker.AddClaim(MakeObserver((uint8_t)PUBLIC_IPV6_CORROBORATION_THRESHOLD), claimed.data()));

	ASSERT_TRUE(tracker.IsCorroborated());
	const uint8_t *corroborated = tracker.CorroboratedAddress();
	ASSERT_TRUE(corroborated != nullptr);
	ASSERT_EQUALS(0, memcmp(corroborated, claimed.data(), claimed.size()));
}

// One host repeating itself is one host. If the count were of claims rather
// than of distinct sources, a single peer could reach any threshold alone,
// which is exactly the property that makes the threshold meaningless.
TEST(PublicIPv6Corroboration, OneObserverRepeatingItselfNeverCorroborates)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address claimed = MakeAddress(0x01);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD + 5; ++i) {
		ASSERT_FALSE(tracker.AddClaim(MakeObserver(7), claimed.data()));
	}
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}

// Agreement is per value. Peers that disagree corroborate nothing, however
// many of them there are -- and the addresses here differ in one byte only.
TEST(PublicIPv6Corroboration, DisagreeingPeersDoNotPoolIntoAQuorum)
{
	CPublicIPv6Corroboration tracker;

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD + 2; ++i) {
		const CPublicIPv6Corroboration::Address claimed = MakeAddress((uint8_t)(i + 1));
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data());
	}

	ASSERT_FALSE(tracker.IsCorroborated());
}

// A quorum for one value is not disturbed by a louder minority for another.
TEST(PublicIPv6Corroboration, AMinorityClaimDoesNotUnseatAQuorum)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address agreed = MakeAddress(0x01);
	const CPublicIPv6Corroboration::Address other = MakeAddress(0x02);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), agreed.data());
	}
	tracker.AddClaim(MakeObserver(100), other.data());
	tracker.AddClaim(MakeObserver(101), other.data());

	ASSERT_TRUE(tracker.IsCorroborated());
	ASSERT_EQUALS(0, memcmp(tracker.CorroboratedAddress(), agreed.data(), agreed.size()));
}

// With no observed source address there is nothing to key on, and an unkeyed
// claim would let one caller supply the whole quorum by itself. Ignored, not
// counted under a zero key.
TEST(PublicIPv6Corroboration, ClaimsWithNoObservedAddressAreIgnored)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address claimed = MakeAddress(0x01);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD + 3; ++i) {
		ASSERT_FALSE(tracker.AddClaim(0, claimed.data()));
	}
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
}

// A NULL payload is a tag that failed to decode, not a claim.
TEST(PublicIPv6Corroboration, NullClaimsAreIgnored)
{
	CPublicIPv6Corroboration tracker;

	ASSERT_FALSE(tracker.AddClaim(MakeObserver(1), nullptr));
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(nullptr));
}

// The set of tracked values is attacker-chosen, so it is bounded. Past the
// bound new values are dropped rather than evicting one that may be honest.
TEST(PublicIPv6Corroboration, CandidateTrackingIsBounded)
{
	CPublicIPv6Corroboration tracker;

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_MAX_CANDIDATES + 20; ++i) {
		const CPublicIPv6Corroboration::Address claimed = MakeAddress((uint8_t)(i + 1));
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data());
	}

	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_MAX_CANDIDATES, (unsigned)tracker.CandidateCount());
	ASSERT_FALSE(tracker.IsCorroborated());
}

// A full bucket does not stop an existing candidate from gaining observers:
// the honest value is usually the first one seen, and it has to be able to
// reach the threshold while noise fills the rest of the table.
TEST(PublicIPv6Corroboration, AnExistingCandidateStillGrowsWhileTheTableIsFull)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address agreed = MakeAddress(0xFF);

	tracker.AddClaim(MakeObserver(1), agreed.data());
	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_MAX_CANDIDATES + 20; ++i) {
		const CPublicIPv6Corroboration::Address noise = MakeAddress((uint8_t)(i + 1));
		tracker.AddClaim(MakeObserver((uint8_t)(i + 50)), noise.data());
	}

	for (unsigned i = 1; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), agreed.data());
	}

	ASSERT_TRUE(tracker.IsCorroborated());
	ASSERT_EQUALS(0, memcmp(tracker.CorroboratedAddress(), agreed.data(), agreed.size()));
}

// Reset drops the quorum, so a tracker cannot carry a stale belief across
// whatever lifetime a caller gives it.
TEST(PublicIPv6Corroboration, ResetForgetsEverything)
{
	CPublicIPv6Corroboration tracker;
	const CPublicIPv6Corroboration::Address claimed = MakeAddress(0x01);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data());
	}
	ASSERT_TRUE(tracker.IsCorroborated());

	tracker.Reset();
	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}
