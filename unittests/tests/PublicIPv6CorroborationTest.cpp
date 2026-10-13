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

// CT_MOD_YOUR_IP (0xAD) carries a peer's opinion of our own public address. eMuleAI believes the
// first peer that offers one; emule-qt does not, and neither does this.
//
// Two separate rules are under test, and they carry different weight:
//
//  - Peers are unauthenticated, so a claim for an address this machine does not hold must never be
// believed however many peers repeat it. That is the security control, and it bounds the damage to
// "confirms the wrong one of our own prefixes".
//  - Among addresses we do hold, one voice is never enough, one /64 cannot be several voices by
// rotating its suffix, a peer that cannot see our IPv6 address is no voice at all, and a voice goes
// quiet after a while.
//
// The rules have no runtime signal if they are wrong. A client that accepts a bad address for
// itself does not fail -- it goes on working, publishing an address nobody can reach -- so the
// tests have to state the rule rather than wait for a symptom.
//
// CUpDownClient reaches theApp and cannot be linked into a unit test, so the tracker lives in a
// header of its own, like CPeerCapabilities.

#include <muleunit/test.h>

#include <PublicIPv6Corroboration.h>

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(PublicIPv6Corroboration)

namespace
{

typedef CPublicIPv6Corroboration::Address Address;

// 2a02:8070:1:<subnet>::<last>. Ours sits in subnet 2; a peer's view of a NATed host lands in
// another. Globally routable, so a peer inside our own /64 is one the observer rule has to reject
// for being inside it, not for its address family.
Address InPrefix(uint8_t subnet, uint8_t last)
{
	Address address = {};
	address[0] = 0x2a;
	address[1] = 0x02;
	address[2] = 0x80;
	address[3] = 0x70;
	address[5] = 0x01;
	address[7] = subnet;
	address[15] = last;
	return address;
}

const uint8_t OUR_SUBNET = 2;
const uint8_t OTHER_SUBNET = 3;

Address Stable()
{
	return InPrefix(OUR_SUBNET, 0x01);
}

Address Temporary()
{
	return InPrefix(OUR_SUBNET, 0x7e);
}

CNetworkAddress AsNetworkAddress(const Address &address)
{
	return CNetworkAddress::IPv6FromOctets(address);
}

// A globally routable IPv6 peer: 2a03:2880:<network>:<subnet>::<host>. Each network is a /48 of
// its own, one subscriber's delegation; subnets and hosts inside one network are that subscriber
// switching /64 or rotating its suffix. Not 2001:db8::/32, which is documentation space and not
// globally routable.
CNetworkAddress Observer(uint8_t network, uint8_t host = 1, uint8_t subnet = 0)
{
	CNetworkAddress::Octets octets = {};
	octets[0] = 0x2a;
	octets[1] = 0x03;
	octets[2] = 0x28;
	octets[3] = 0x80;
	octets[5] = network;
	octets[7] = subnet;
	octets[15] = host;
	return CNetworkAddress::IPv6FromOctets(octets);
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
	return std::vector<Address>{ one };
}

std::vector<Address> Held(const Address &one, const Address &two)
{
	return std::vector<Address>{ one, two };
}

// An arbitrary non-zero start, so a test that forgets to advance the clock
// cannot pass by accident on a zero-initialised timestamp.
const uint64_t START_MS = 5000;

// Claims @a claimed from observer networks 1..count.
void ClaimFrom(CPublicIPv6Corroboration &tracker, unsigned count, const Address &claimed, uint64_t nowMs)
{
	for (unsigned i = 0; i < count; ++i) {
		tracker.AddClaim(Observer((uint8_t)(i + 1)), claimed.data(), nowMs);
	}
}

} // namespace

// The threshold is a policy decision, not an implementation detail: raising or lowering it changes
// how much a stranger's word is worth. Pinned as a literal so the change has to be deliberate.
TEST(PublicIPv6Corroboration, ThresholdIsThreeDistinctObservers)
{
	ASSERT_EQUALS(3u, (unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD);
}

// Same for the window: it is the upper bound on how long a claim nobody repeats keeps counting, and
// shortening or lengthening it changes how long a stale address can survive.
TEST(PublicIPv6Corroboration, WindowIsThirtyMinutes)
{
	ASSERT_EQUALS((uint64_t)(30 * 60 * 1000), (uint64_t)PUBLIC_IPV6_CORROBORATION_WINDOW_MS);
}

TEST(PublicIPv6Corroboration, ThreeObserverPrefixesCorroborateOurPrefix)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);

	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// The return value is the edge, not the level: true on the claim that completes the quorum and on
// no other, so a caller logging it logs once.
TEST(PublicIPv6Corroboration, AddClaimReportsReachingTheThresholdExactlyOnce)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	ASSERT_FALSE(tracker.AddClaim(Observer(1), Stable().data(), START_MS));
	ASSERT_FALSE(tracker.AddClaim(Observer(2), Stable().data(), START_MS));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
	ASSERT_TRUE(tracker.AddClaim(Observer(3), Stable().data(), START_MS));
	ASSERT_FALSE(tracker.AddClaim(Observer(3), Stable().data(), START_MS));
	ASSERT_FALSE(tracker.AddClaim(Observer(4), Stable().data(), START_MS));
}

// A subscriber holds a whole /64 and can source each hello from a fresh suffix. Counted per /128,
// one host would be the whole quorum.
TEST(PublicIPv6Corroboration, AddressesInsideOneObserverPrefixCountOnce)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	for (unsigned host = 1; host <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++host) {
		ASSERT_FALSE(tracker.AddClaim(Observer(1, (uint8_t)host), Stable().data(), START_MS));
	}

	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// Home subscribers are usually delegated a /56 or a /48, so counted per /64 one subscriber could
// supply the whole quorum from its own subnets.
TEST(PublicIPv6Corroboration, SubnetsOfOneObserverDelegationCountOnce)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	for (unsigned subnet = 1; subnet <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++subnet) {
		ASSERT_FALSE(tracker.AddClaim(Observer(1, 1, (uint8_t)subnet), Stable().data(), START_MS));
	}
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));

	ASSERT_FALSE(tracker.AddClaim(Observer(2), Stable().data(), START_MS));
	ASSERT_TRUE(tracker.AddClaim(Observer(3), Stable().data(), START_MS));
	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// A peer in another /64 of our own /48 is a neighbouring subnet of our own delegation: it sees us
// from inside, not from the outside world.
TEST(PublicIPv6Corroboration, AnObserverInAnotherSubnetOfOurDelegationDoesNotCount)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	const Address neighbour = InPrefix(OTHER_SUBNET, 0x99);
	ASSERT_TRUE(AsNetworkAddress(neighbour).TruncatedToPrefix(48) ==
		    AsNetworkAddress(Stable()).TruncatedToPrefix(48));

	tracker.AddClaim(Observer(1), Stable().data(), START_MS);
	tracker.AddClaim(Observer(2), Stable().data(), START_MS);
	ASSERT_FALSE(tracker.AddClaim(AsNetworkAddress(neighbour), Stable().data(), START_MS));

	ASSERT_EQUALS(2u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// A peer that reached us over IPv4 never saw our IPv6 address. Once we advertise one, it can only
// echo our own advertisement back, which is no witness at all.
TEST(PublicIPv6Corroboration, IPv4AndMappedObserversDoNotCount)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	for (unsigned i = 1; i <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		const uint32_t ipv4 = 0x51024500u | i; // 81.2.69.<i>
		CNetworkAddress::Octets mapped = {};
		mapped[10] = 0xff;
		mapped[11] = 0xff;
		mapped[12] = 81;
		mapped[13] = 2;
		mapped[14] = 70;
		mapped[15] = (uint8_t)i;
		ASSERT_FALSE(tracker.AddClaim(
			CNetworkAddress::FromIPv4HostOrder(ipv4), Stable().data(), START_MS));
		ASSERT_FALSE(
			tracker.AddClaim(CNetworkAddress::IPv6FromOctets(mapped), Stable().data(), START_MS));
	}

	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// A unique-local or link-local peer is on a network of ours, so it cannot tell us how the outside
// world sees us.
TEST(PublicIPv6Corroboration, UniqueLocalAndLinkLocalObserversDoNotCount)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	for (unsigned i = 1; i <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		Address uniqueLocal = MakePrefixed(0xfd, 0x00);
		uniqueLocal[5] = (uint8_t)i;
		uniqueLocal[15] = 1;
		Address linkLocal = MakePrefixed(0xfe, 0x80);
		linkLocal[15] = (uint8_t)i;
		ASSERT_FALSE(tracker.AddClaim(AsNetworkAddress(uniqueLocal), Stable().data(), START_MS));
		ASSERT_FALSE(tracker.AddClaim(
			CNetworkAddress::IPv6FromOctets(linkLocal, 2), Stable().data(), START_MS));
	}

	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// A host on our own LAN prefix, or a container on the same bridge, sees our address directly. It
// says nothing about whether anyone outside can.
TEST(PublicIPv6Corroboration, AnObserverInsideTheClaimedPrefixDoesNotCount)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	tracker.AddClaim(Observer(1), Stable().data(), START_MS);
	tracker.AddClaim(Observer(2), Stable().data(), START_MS);
	ASSERT_FALSE(
		tracker.AddClaim(AsNetworkAddress(InPrefix(OUR_SUBNET, 0x99)), Stable().data(), START_MS));

	ASSERT_EQUALS(2u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// Outbound connections leave from the temporary address, so that is what most peers report; the
// stable address is the one we would advertise. Both are in one /64, and the votes are for it.
TEST(PublicIPv6Corroboration, VotesForTemporaryAndStableAddressesOfOnePrefixAddUp)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable(), Temporary()), START_MS);

	ASSERT_FALSE(tracker.AddClaim(Observer(1), Temporary().data(), START_MS));
	ASSERT_FALSE(tracker.AddClaim(Observer(2), Temporary().data(), START_MS));
	ASSERT_TRUE(tracker.AddClaim(Observer(3), Stable().data(), START_MS));

	ASSERT_EQUALS(1u, (unsigned)tracker.CandidateCount());
	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// The security control, and the Docker NAT case: the container holds a global address nobody can
// reach, and every peer reports the host's. However many agree, an address we do not hold is never
// believed, and our own prefix gains nothing from it.
TEST(PublicIPv6Corroboration, AClaimForAnAddressWeDoNotHoldNeverCorroborates)
{
	CPublicIPv6Corroboration tracker;
	const Address host = InPrefix(OTHER_SUBNET, 0x01);
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	for (unsigned i = 1; i <= PUBLIC_IPV6_CORROBORATION_MAX_OBSERVERS + 4; ++i) {
		ASSERT_FALSE(tracker.AddClaim(Observer((uint8_t)i), host.data(), START_MS));
	}

	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(host), START_MS));
}

// A rejected claim must cost nothing to hold. If it allocated a candidate, a peer could spend our
// memory on values it invented at one claim each, which is the whole reason the filter comes first.
// Inside our own /64 too: the gate is on the exact address, not on the prefix.
TEST(PublicIPv6Corroboration, ARejectedClaimLeavesNoTrace)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(InPrefix(OUR_SUBNET, 0xff)), START_MS);

	for (unsigned i = 0; i < 255; ++i) {
		const Address invented = InPrefix(OUR_SUBNET, (uint8_t)i);
		tracker.AddClaim(Observer((uint8_t)i), invented.data(), START_MS);
	}

	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
}

// The query is about an address of ours. A corroborated /64 says nothing about another address in
// it, and nothing at all about an IPv4 or mapped address.
TEST(PublicIPv6Corroboration, TheQueryIsFalseForAnAddressWeDoNotHold)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);
	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));

	CNetworkAddress::Octets mapped = {};
	mapped[10] = 0xff;
	mapped[11] = 0xff;
	mapped[12] = 81;
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(InPrefix(OUR_SUBNET, 0x42)), START_MS));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(CNetworkAddress::FromIPv4HostOrder(0x51024501u), START_MS));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(CNetworkAddress::IPv6FromOctets(mapped), START_MS));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(CNetworkAddress::Absent(), START_MS));
}

// Nothing outside 2000::/3 could be an address the outside world saw us arrive from, so none of it
// is a legitimate reflection -- not even if the same bytes somehow turned up in the locally-
// assigned list.
TEST(PublicIPv6Corroboration, AddressesOutsideGlobalUnicastAreRejected)
{
	const Address unspecified = MakePrefixed(0x00, 0x00);
	Address loopback = MakePrefixed(0x00, 0x00);
	loopback[15] = 0x01;
	const Address linkLocal = MakePrefixed(0xfe, 0x80);
	const Address uniqueLocal = MakePrefixed(0xfd, 0x00);
	const Address multicast = MakePrefixed(0xff, 0x02);

	const std::vector<Address> rejected{ unspecified, loopback, linkLocal, uniqueLocal, multicast };
	for (const Address &value : rejected) {
		ASSERT_FALSE(CPublicIPv6Corroboration::IsGlobalUnicast(value));

		CPublicIPv6Corroboration tracker;
		// Handed in as "local" on purpose: the address-family gate is an invariant of the
		// tracker, not a side effect of how the caller happened to fill that set.
		tracker.SetLocalAddresses(Held(value), START_MS);
		ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, value, START_MS);
		ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
		ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(value), START_MS));
	}
}

// Before the interface list has been published there is nothing to check a claim against, and the
// safe reading of "I do not know which addresses I hold" is "believe nobody".
TEST(PublicIPv6Corroboration, WithNoPublishedInterfacesNothingIsBelieved)
{
	CPublicIPv6Corroboration tracker;

	for (unsigned i = 1; i <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		ASSERT_FALSE(tracker.AddClaim(Observer((uint8_t)i), Stable().data(), START_MS));
	}
	ASSERT_EQUALS(0u, (unsigned)tracker.LocalAddressCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
}

// The point of keying on the observed source: repetition is not corroboration, however many times
// it happens.
TEST(PublicIPv6Corroboration, OneObserverRepeatingItselfNeverCorroborates)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD * 10; ++i) {
		ASSERT_FALSE(tracker.AddClaim(Observer(7), Stable().data(), START_MS));
	}
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
}

// Votes are counted per /64, never in total: three peers naming three of our prefixes agree about
// nothing.
TEST(PublicIPv6Corroboration, DisagreeingPeersDoNotPoolIntoAQuorum)
{
	CPublicIPv6Corroboration tracker;
	std::vector<Address> local;
	for (unsigned i = 1; i <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		local.push_back(InPrefix((uint8_t)(0x10 + i), 0x01));
	}
	tracker.SetLocalAddresses(local, START_MS);

	for (unsigned i = 0; i < local.size(); ++i) {
		tracker.AddClaim(Observer((uint8_t)(i + 1)), local[i].data(), START_MS);
	}

	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD, (unsigned)tracker.CandidateCount());
	for (const Address &address : local) {
		ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(address), START_MS));
	}
}

// The reason the cap sits above the quorum. Three peers corroborate a prefix, three more agree
// later, and when the first three fall out of the window the prefix is still held up by the
// others. With the cap equal to the quorum those later votes were refused, so the first expiry
// dropped the candidate outright even though half the network was still agreeing.
TEST(PublicIPv6Corroboration, SurplusVotesCarryAPrefixThroughAnExpiry)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);

	const uint64_t later = START_MS + 1000;
	for (unsigned i = 1; i <= PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		tracker.AddClaim(
			Observer((uint8_t)(i + PUBLIC_IPV6_CORROBORATION_THRESHOLD)), Stable().data(), later);
	}
	ASSERT_EQUALS((unsigned)(PUBLIC_IPV6_CORROBORATION_THRESHOLD * 2),
		(unsigned)tracker.DistinctObserversFor(Stable().data()));

	// Past the window for the first set, inside it for the second.
	const uint64_t afterFirstSet = START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 500;
	tracker.SetLocalAddresses(Held(Stable()), afterFirstSet);

	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), afterFirstSet));
	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD,
		(unsigned)tracker.DistinctObserversFor(Stable().data()));
}

// The cap is still a cap. Surplus is redundancy, not an unbounded list a peer can grow.
TEST(PublicIPv6Corroboration, ANinthObserverPrefixIsNotStored)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_MAX_OBSERVERS + 1, Stable(), START_MS);

	ASSERT_EQUALS(8u, (unsigned)PUBLIC_IPV6_CORROBORATION_MAX_OBSERVERS);
	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_MAX_OBSERVERS,
		(unsigned)tracker.DistinctObserversFor(Stable().data()));
}

TEST(PublicIPv6Corroboration, NullClaimsAndAbsentObserversAreIgnored)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);

	ASSERT_FALSE(tracker.AddClaim(Observer(1), nullptr, START_MS));
	for (unsigned i = 0; i < PUBLIC_IPV6_CORROBORATION_THRESHOLD; ++i) {
		ASSERT_FALSE(tracker.AddClaim(CNetworkAddress::Absent(), Stable().data(), START_MS));
	}
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(nullptr));
}

// A vote nobody has repeated within the window is not current evidence. This is the difference
// between "three peers agree" and "three peers said so at some point since the daemon started".
TEST(PublicIPv6Corroboration, VotesOlderThanTheWindowStopCounting)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD - 1, Stable(), START_MS);

	// The deciding claim arrives one millisecond after the first two aged out.
	const uint64_t tooLate = START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 1;
	ASSERT_FALSE(tracker.AddClaim(
		Observer((uint8_t)PUBLIC_IPV6_CORROBORATION_THRESHOLD), Stable().data(), tooLate));
	ASSERT_EQUALS(1u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
}

// A quorum that formed and then went quiet lapses at read time. Nothing has to arrive to age it out,
// or a client whose peers all left would keep advertising on stale evidence.
TEST(PublicIPv6Corroboration, TheQueryHonoursTheWindowWithoutANewClaim)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);

	const CNetworkAddress stable = AsNetworkAddress(Stable());
	ASSERT_TRUE(tracker.IsPrefixCorroborated(stable, START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS));
	ASSERT_FALSE(
		tracker.IsPrefixCorroborated(stable, START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 1));
	// A clock that appears to run backwards does not keep a vote alive either.
	ASSERT_FALSE(tracker.IsPrefixCorroborated(stable, START_MS - 1));
}

// A peer that is still saying the same thing keeps its vote. Without this the window would date
// every vote from the first hello, so a stable client whose peers never stopped agreeing would
// still lose its quorum once, on the clock, for no reason.
TEST(PublicIPv6Corroboration, ARepeatingObserverKeepsItsVoteAlive)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);

	const uint64_t repeated = START_MS + (PUBLIC_IPV6_CORROBORATION_WINDOW_MS * 3) / 5;
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), repeated);

	// Past the window measured from the first hello, inside it measured from the repeat.
	const uint64_t afterFirstHello = START_MS + PUBLIC_IPV6_CORROBORATION_WINDOW_MS + 1000;
	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), afterFirstHello));
	// Repetition renews a vote; it never adds one.
	ASSERT_EQUALS((unsigned)PUBLIC_IPV6_CORROBORATION_THRESHOLD,
		(unsigned)tracker.DistinctObserversFor(Stable().data()));
}

// The half of the refresh that makes the cache safe: a prefix that left the interface takes its
// corroboration with it. Without this a reflection outlives the prefix it came from.
TEST(PublicIPv6Corroboration, ARenumberDropsTheCorroboratedPrefix)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);
	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));

	tracker.SetLocalAddresses(Held(InPrefix(OTHER_SUBNET, 0x01)), START_MS + 1000);

	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS + 1000));
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(0u, (unsigned)tracker.DistinctObserversFor(Stable().data()));
}

// A privacy-address rotation is not a renumber: the /64 is still ours, so its votes stay.
TEST(PublicIPv6Corroboration, ARotatedTemporaryAddressKeepsItsPrefixVotes)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable(), Temporary()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Temporary(), START_MS);

	tracker.SetLocalAddresses(Held(Stable(), InPrefix(OUR_SUBNET, 0x7f)), START_MS + 1000);

	ASSERT_TRUE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS + 1000));
}

// Reset drops what peers said. It deliberately does not drop the interface list: that comes from
// this machine, and clearing it would leave the filter wide open until the next refresh.
TEST(PublicIPv6Corroboration, ResetForgetsClaimsButKeepsOurOwnAddresses)
{
	CPublicIPv6Corroboration tracker;
	tracker.SetLocalAddresses(Held(Stable()), START_MS);
	ClaimFrom(tracker, PUBLIC_IPV6_CORROBORATION_THRESHOLD, Stable(), START_MS);

	tracker.Reset();

	ASSERT_FALSE(tracker.IsPrefixCorroborated(AsNetworkAddress(Stable()), START_MS));
	ASSERT_EQUALS(0u, (unsigned)tracker.CandidateCount());
	ASSERT_EQUALS(1u, (unsigned)tracker.LocalAddressCount());
}
