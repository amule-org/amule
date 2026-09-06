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
// neither does this.
//
// Two separate rules are under test, and they carry different weight:
//
//  - Peers are unauthenticated, so a claim for an address this machine does
//    not hold must never be believed no matter how many peers repeat it. That
//    is the security control, and it is what bounds the damage to "picks the
//    wrong one of our own addresses".
//  - Among addresses we do hold, one voice is never enough, one host cannot be
//    several voices by repeating itself, and a voice goes quiet after a while.
//
// The rules have no runtime signal if they are wrong. A client that accepts a
// bad address for itself does not fail -- it goes on working, publishing an
// address nobody can reach, so the tests have to state the rule rather than
// wait for a symptom.
//
// CUpDownClient reaches theApp and cannot be linked into a unit test, so the
// tracker lives in a header of its own, like CPeerCapabilities.

#include <muleunit/test.h>

#include <PublicIPv6Corroboration.h>

#include <cstring>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(PublicIPv6Corroboration)

namespace
{

typedef CPublicIPv6Corroboration::Address Address;

// 2001:db8::<last>, the documentation prefix -- global unicast, so it passes
// the address-family gate and only the locally-assigned gate is under test.
// The last byte is the only thing that varies, so two addresses differ in a
// single byte -- the case a byte-wise comparison is most likely to get wrong.
Address MakeAddress(uint8_t last)
{
	Address address = {};
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

// An address with the given leading bytes and nothing else set. Used for the
// prefixes that are not global unicast, which no peer may ever reflect at us.
Address MakePrefixed(uint8_t first, uint8_t second)
{
	Address address = {};
	address[0] = first;
	address[1] = second;
	return address;
}

std::vector<Address> Held(const Address &one)
{
	std::vector<Address> local;
	local.push_back(one);
	return local;
}

std::vector<Address> Held(const Address &one, const Address &two)
{
	std::vector<Address> local;
	local.push_back(one);
	local.push_back(two);
	return local;
}

// An arbitrary non-zero start, so a test that forgets to advance the clock
// cannot pass by accident on a zero-initialised timestamp.
const uint64_t START_MS = 5000;

} // namespace

// The threshold is a policy decision, not an implementation detail: raising or
// lowering it changes how much a stranger's word is worth. Pinned as a literal
// so the change has to be deliberate.
TEST(PublicIPv6Corroboration, ThresholdIsThreeDistinctObservers)
{
	ASSERT_EQUALS(3u, (unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD);
}

// Same for the window: it is the upper bound on how long a claim nobody
// repeats keeps counting, and shortening or lengthening it changes how long a
// stale address can survive.
TEST(PublicIPv6Corroboration, WindowIsThirtyMinutes)
{
	ASSERT_EQUALS((uint64_t)(30 * 60 * 1000), (uint64_t)PUBLIC_IPV6_CORROBORATION_WINDOW_MS);
}

// The security control. A quorum -- more than the threshold, from distinct
// observers, all agreeing -- must still not make us adopt an address this
// machine does not hold. Corroboration disambiguates between our own
// addresses; it does not confer ownership of somebody else's.
TEST(PublicIPv6Corroboration, AQuorumCannotAdoptAnAddressWeDoNotHold)
{
	CPublicIPv6Corroboration tracker;
	const Address ours = MakeAddress(0x01);
	const Address foreign = MakeAddress(0x02);
	tracker.SetLocalAddresses(Held(ours), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD + 5; ++i) {
		ASSERT_FALSE(tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), foreign.data(), START_MS));
	}

	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_TRUE(tracker.CorroboratedAddress() == nullptr);
}

// A rejected claim must cost nothing to hold. If it allocated a candidate, a
// peer could spend our memory on values it invented at one claim each, which
// is the whole reason the filter comes first.
TEST(PublicIPv6Corroboration, ARejectedClaimLeavesNoTrace)
{
	CPublicIPv6Corroboration tracker;
	const Address ours = MakeAddress(0xFF);
	tracker.SetLocalAddresses(Held(ours), START_MS);

	for (unsigned i = 0; i < 256; ++i) {
		const Address invented = MakeAddress((uint8_t)i);
		if (invented == ours) {
			continue;
		}
		tracker.AddClaim(MakeObserver((uint8_t)i), invented.data(), START_MS);
	}

	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_FALSE(tracker.IsCorroborated());
}

// Nothing outside 2000::/3 could be an address the outside world saw us arrive
// from, so none of it is a legitimate reflection -- not even if the same bytes
// somehow turned up in the locally-assigned list.
TEST(PublicIPv6Corroboration, AddressesOutsideGlobalUnicastAreRejected)
{
	const Address unspecified = MakePrefixed(0x00, 0x00);
	Address loopback = MakePrefixed(0x00, 0x00);
	loopback[15] = 0x01;
	const Address linkLocal = MakePrefixed(0xfe, 0x80);
	const Address uniqueLocal = MakePrefixed(0xfd, 0x00);
	const Address multicast = MakePrefixed(0xff, 0x02);

	std::vector<Address> rejected;
	rejected.push_back(unspecified);
	rejected.push_back(loopback);
	rejected.push_back(linkLocal);
	rejected.push_back(uniqueLocal);
	rejected.push_back(multicast);

	for (const Address &value : rejected) {
		ASSERT_FALSE(CPublicIPv6Corroboration::IsGlobalUnicast(value));

		CPublicIPv6Corroboration tracker;
		// Handed in as "local" on purpose: the address-family gate is an
		// invariant of the tracker, not a side effect of how the caller
		// happened to fill that set.
		tracker.SetLocalAddresses(Held(value), START_MS);
		for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
			tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), value.data(), START_MS);
		}
		ASSERT_FALSE(tracker.IsCorroborated());
		ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	}
}

// Before the interface list has been published there is nothing to check a
// claim against, and the safe reading of "I do not know which addresses I
// hold" is "believe nobody".
TEST(PublicIPv6Corroboration, WithNoPublishedInterfacesNothingIsBelieved)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		ASSERT_FALSE(tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS));
	}
	ASSERT_EQUALS(0u, (unsigned)tracker.LocalAddressCount());
}

// One peer is one peer, even about an address we do hold.
TEST(PublicIPv6Corroboration, OnePeerIsNotBelieved)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	ASSERT_FALSE(tracker.AddClaim(MakeObserver(1), claimed.data(), START_MS));
	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_TRUE(tracker.CorroboratedAddress() == nullptr);
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}

// Two source addresses is one dual-homed host, one new lease, or one attacker
// with a second socket -- not a second opinion.
TEST(PublicIPv6Corroboration, TwoPeersAreStillNotEnough)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	tracker.AddClaim(MakeObserver(1), claimed.data(), START_MS);
	ASSERT_FALSE(tracker.AddClaim(MakeObserver(2), claimed.data(), START_MS));
	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_EQUALS(2u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}

TEST(PublicIPv6Corroboration, DistinctObserversReachingTheThresholdCorroborate)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i + 1 < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		ASSERT_FALSE(tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS));
	}
	ASSERT_TRUE(tracker.AddClaim(
		MakeObserver((uint8_t)PUBLIC_IPV6_CORROBORATION_THRESHOLD), claimed.data(), START_MS));

	ASSERT_TRUE(tracker.IsCorroborated());
	ASSERT_TRUE(tracker.CorroboratedAddress() != nullptr);
	ASSERT_EQUALS(0, std::memcmp(tracker.CorroboratedAddress(), claimed.data(), claimed.size()));
}

// The point of keying on the observed source address: repetition is not
// corroboration, however many times it happens.
TEST(PublicIPv6Corroboration, OneObserverRepeatingItselfNeverCorroborates)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD * 10; ++i) {
		ASSERT_FALSE(tracker.AddClaim(MakeObserver(7), claimed.data(), START_MS));
	}
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}

// Votes are counted per value, never in total: three peers naming three
// different addresses agree about nothing.
TEST(PublicIPv6Corroboration, DisagreeingPeersDoNotPoolIntoAQuorum)
{
	CPublicIPv6Corroboration tracker;
	std::vector<Address> local;
	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		local.push_back(MakeAddress((uint8_t)(i + 1)));
	}
	tracker.SetLocalAddresses(local, START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		const Address claimed = MakeAddress((uint8_t)(i + 1));
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS);
	}

	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD, (unsigned)tracker.CandidateCount());
}

// A seated value is not unseated by a minority. Both addresses here are ours,
// so the loser is not a threat -- but flipping between them on every packet
// would be a different address published every few seconds.
TEST(PublicIPv6Corroboration, AMinorityClaimDoesNotUnseatAQuorum)
{
	CPublicIPv6Corroboration tracker;
	const Address agreed = MakeAddress(0x01);
	const Address other = MakeAddress(0x02);
	tracker.SetLocalAddresses(Held(agreed, other), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), agreed.data(), START_MS);
	}
	tracker.AddClaim(MakeObserver(100), other.data(), START_MS);
	tracker.AddClaim(MakeObserver(101), other.data(), START_MS);

	ASSERT_TRUE(tracker.IsCorroborated());
	ASSERT_EQUALS(0, std::memcmp(tracker.CorroboratedAddress(), agreed.data(), agreed.size()));
}

// Without an observed address there is nothing to key on, and an unkeyed claim
// would let one peer supply the whole quorum by itself.
TEST(PublicIPv6Corroboration, ClaimsWithNoObservedAddressAreIgnored)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD * 5; ++i) {
		ASSERT_FALSE(tracker.AddClaim(0, claimed.data(), START_MS));
	}
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
}

TEST(PublicIPv6Corroboration, NullClaimsAreIgnored)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(MakeAddress(0x01)), START_MS);

	ASSERT_FALSE(tracker.AddClaim(MakeObserver(1), nullptr, START_MS));
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(nullptr));
}

// The filter is what bounds the table: a candidate can only ever be one of the
// addresses this machine holds, so a peer flooding invented values cannot make
// the table grow at all -- which is why there is no cap on it any more.
TEST(PublicIPv6Corroboration, TheCandidateSetIsBoundedByOurOwnAddresses)
{
	CPublicIPv6Corroboration tracker;
	const Address first = MakeAddress(0x01);
	const Address second = MakeAddress(0x02);
	tracker.SetLocalAddresses(Held(first, second), START_MS);

	for (unsigned i = 0; i < 250; ++i) {
		const Address claimed = MakeAddress((uint8_t)i);
		tracker.AddClaim(MakeObserver((uint8_t)i), claimed.data(), START_MS);
	}

	ASSERT_EQUALS(2u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(2u, (unsigned)tracker.LocalAddressCount());
}

// A vote nobody has repeated within the window is not current evidence. This
// is the difference between "three peers agree" and "three peers said so at
// some point since the daemon started".
TEST(PublicIPv6Corroboration, VotesOlderThanTheWindowStopCounting)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i + 1 < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS);
	}

	// The deciding claim arrives one millisecond after the first two aged out.
	const uint64_t tooLate = START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 1;
	ASSERT_FALSE(tracker.AddClaim(
		MakeObserver((uint8_t)PUBLIC_IPV6_CORROBORATION_THRESHOLD), claimed.data(), tooLate));
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
}

// A quorum that formed and then went quiet must lapse too, otherwise the
// window only ever applies to claims that never made it.
TEST(PublicIPv6Corroboration, AnAdoptedValueLapsesWhenItsVotesAgeOut)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS);
	}
	ASSERT_TRUE(tracker.IsCorroborated());

	// No peer said anything since; only the periodic interface refresh ran.
	tracker.SetLocalAddresses(Held(claimed), START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 1);
	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_TRUE(tracker.CorroboratedAddress() == nullptr);
}

// A peer that is still connected and still saying the same thing keeps its
// vote. Without this the window would date every vote from the first hello, so
// a stable client whose peers never stopped agreeing would still lose its
// quorum once, on the clock, for no reason.
TEST(PublicIPv6Corroboration, ARepeatingObserverKeepsItsVoteAlive)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS);
	}
	ASSERT_TRUE(tracker.IsCorroborated());

	// The same peers say it again, comfortably inside the window.
	const uint64_t repeated = START_MS + (PUBLIC_IPV6_CORROBORATION_WINDOW_MS * 3) / 5;
	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), repeated);
	}

	// Past the window measured from the first hello, inside it measured from
	// the repeat. Only the interface refresh runs here, so nothing is re-added.
	tracker.SetLocalAddresses(Held(claimed), START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 1000);

	ASSERT_TRUE(tracker.IsCorroborated());
	// Repetition renews a vote; it never adds one.
	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD,
		(unsigned)tracker.DistinctObserversFor(claimed.data()));
}

// The half of the refresh that makes the cache safe: an address that left the
// interface takes its corroboration with it. Without this a reflection
// outlives the prefix it came from.
TEST(PublicIPv6Corroboration, ARenumberDropsTheAdoptedAddress)
{
	CPublicIPv6Corroboration tracker;
	const Address oldPrefix = MakeAddress(0x01);
	const Address newPrefix = MakeAddress(0x02);
	tracker.SetLocalAddresses(Held(oldPrefix), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), oldPrefix.data(), START_MS);
	}
	ASSERT_TRUE(tracker.IsCorroborated());

	tracker.SetLocalAddresses(Held(newPrefix), START_MS + 1000);

	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(oldPrefix.data()));
}

// After the old value loses its seat, the next value to hold a quorum takes
// it. A tracker that could only ever elect once would be stuck on the first
// address it saw for the life of the process.
TEST(PublicIPv6Corroboration, AnotherAddressIsElectedAfterTheFirstLosesItsSeat)
{
	CPublicIPv6Corroboration tracker;
	const Address oldPrefix = MakeAddress(0x01);
	const Address newPrefix = MakeAddress(0x02);
	tracker.SetLocalAddresses(Held(oldPrefix), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), oldPrefix.data(), START_MS);
	}
	ASSERT_EQUALS(0, std::memcmp(tracker.CorroboratedAddress(), oldPrefix.data(), oldPrefix.size()));

	const uint64_t later = START_MS + 1000;
	tracker.SetLocalAddresses(Held(newPrefix), later);
	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), newPrefix.data(), later);
	}

	ASSERT_TRUE(tracker.IsCorroborated());
	ASSERT_EQUALS(0, std::memcmp(tracker.CorroboratedAddress(), newPrefix.data(), newPrefix.size()));
}

// Reset drops what peers said. It deliberately does not drop the interface
// list: that comes from this machine, and clearing it would leave the filter
// wide open until the next refresh.
TEST(PublicIPv6Corroboration, ResetForgetsClaimsButKeepsOurOwnAddresses)
{
	CPublicIPv6Corroboration tracker;
	const Address claimed = MakeAddress(0x01);
	tracker.SetLocalAddresses(Held(claimed), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(MakeObserver((uint8_t)(i + 1)), claimed.data(), START_MS);
	}
	ASSERT_TRUE(tracker.IsCorroborated());

	tracker.Reset();
	ASSERT_FALSE(tracker.IsCorroborated());
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(claimed.data()));
	ASSERT_EQUALS(1u, (unsigned)tracker.LocalAddressCount());
}
