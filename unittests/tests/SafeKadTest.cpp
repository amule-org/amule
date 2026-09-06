//								-*- C++ -*-
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
#include <kademlia/net/SafeKad.h>
#include <protocol/kad2/Constants.h>

using namespace muleunit;
using Kademlia::CSafeKad;
using Kademlia::CUInt128;

// A fixed "now" for every test; the class takes the time as a parameter, so no
// test here waits on a real clock.
static const time_t T0 = 1700000000;

static const uint32_t IP_A = 0x0A000001;
static const uint32_t IP_B = 0x0A000002;
static const uint16_t PORT_A = 4672;
static const uint16_t PORT_B = 4673;

static CUInt128 Id(uint32_t seed)
{
	return CUInt128(seed);
}

DECLARE_SIMPLE(SafeKad)

TEST(SafeKad, FirstSightingOfANodeIsAccepted)
{
	CSafeKad safe;
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, true, T0));
	ASSERT_EQUALS(1u, (unsigned)safe.GetTrackedNodeCount());
	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, T0));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0));
}

TEST(SafeKad, SameIdentityIsAcceptedRepeatedly)
{
	CSafeKad safe;
	for (unsigned i = 0; i < 20; ++i) {
		ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, true, T0 + i));
	}
	ASSERT_EQUALS(1u, (unsigned)safe.GetTrackedNodeCount());
}

TEST(SafeKad, RapidIdentityRotationIsRejectedAndMarkedProblematic)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));

	// Same address, new ID, well inside the one-hour minimum interval.
	const time_t soon = T0 + CSafeKad::MIN_ID_CHANGE_INTERVAL - 1;
	ASSERT_FALSE(safe.TrackNode(IP_A, PORT_A, Id(2), true, soon));

	// The contact must be rejected...
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), KADEMLIA_VERSION, true, false, soon));
	// ...and the address must be on the problematic list.
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, soon));
}

TEST(SafeKad, IdentityChangeAfterTheIntervalIsAccepted)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));

	const time_t later = T0 + CSafeKad::MIN_ID_CHANGE_INTERVAL + 1;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(2), true, later));
	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, later));
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(2), KADEMLIA_VERSION, true, false, later));
}

TEST(SafeKad, RepeatedRotationEscalatesToABan)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));
	// First rotation: problematic.
	ASSERT_FALSE(safe.TrackNode(IP_A, PORT_A, Id(2), true, T0 + 10));
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, T0 + 10));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 10));

	// Second rotation while still problematic: banned.
	ASSERT_FALSE(safe.TrackNode(IP_A, PORT_A, Id(3), true, T0 + 20));
	ASSERT_TRUE(safe.IsBanned(IP_A, T0 + 20));
	// A banned address stops being tracked; there is nothing left to weigh.
	ASSERT_EQUALS(0u, (unsigned)safe.GetTrackedNodeCount());
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(3), KADEMLIA_VERSION, true, false, T0 + 20));
}

TEST(SafeKad, AnUnverifiedClaimCannotOverwriteAVerifiedIdentity)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));

	// Long past the interval, so the rate limit is not what rejects this.
	const time_t later = T0 + CSafeKad::MIN_ID_CHANGE_INTERVAL * 2;
	ASSERT_FALSE(safe.TrackNode(IP_A, PORT_A, Id(9), false, later));
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(9), KADEMLIA_VERSION, false, false, later));
	// The verified identity is still the one we hold.
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, false, later));
}

TEST(SafeKad, PreVersion8NodesGetNoUnverifiedIdentityChangeAtAll)
{
	CSafeKad safe;
	// Tracked unverified, so the "verified beats unverified" rule is not
	// what is doing the work here.
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), false, T0));

	// Keep the entry's last-reference time fresh while its last identity
	// change ages. That is what a node we keep talking to looks like; without
	// it, NODE_MAX_REFERENCE_AGE would reclaim the entry before the
	// identity-change interval had elapsed and there would be nothing left
	// to reject.
	const time_t later = T0 + CSafeKad::MIN_ID_CHANGE_INTERVAL * 2;
	safe.TrackNode(IP_A, PORT_A, Id(1), false, later - 1);

	// A 0x07 node could not prove which port it listens on.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), 0x07, false, false, later));
	// The same change from a 0x08 node, past the interval, is fine.
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(2), 0x08, true, false, later));
}

TEST(SafeKad, OneNodePerAddressIsEnforcedWhenAsked)
{
	CSafeKad safe;
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, true, T0));

	// A second Kad port on the same address.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_B, Id(2), KADEMLIA_VERSION, true, true, T0));
	// The first port keeps working.
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, true, T0));
	// A different address is unaffected.
	ASSERT_FALSE(safe.IsBadNode(IP_B, PORT_B, Id(3), KADEMLIA_VERSION, true, true, T0));

	// With the check off (the search-response path, where the contact is
	// already in our routing table), the second port is allowed.
	CSafeKad relaxed;
	ASSERT_FALSE(relaxed.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, false, T0));
	ASSERT_FALSE(relaxed.IsBadNode(IP_A, PORT_B, Id(2), KADEMLIA_VERSION, true, false, T0));
}

TEST(SafeKad, ProblematicEntriesExpireAfterTheirHorizon)
{
	CSafeKad safe;
	safe.TrackProblematicNode(IP_A, PORT_A, T0);
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, T0));
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, T0 + CSafeKad::MAX_PROBLEMATIC_TIME));

	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, T0 + CSafeKad::MAX_PROBLEMATIC_TIME + 1));
	ASSERT_EQUALS(0u, (unsigned)safe.GetProblematicNodeCount());
}

TEST(SafeKad, BansLapseAfterFourHours)
{
	CSafeKad safe;
	safe.BanAddress(IP_A, T0);
	ASSERT_TRUE(safe.IsBanned(IP_A, T0));
	ASSERT_TRUE(safe.IsBanned(IP_A, T0 + CSafeKad::MAX_BAN_TIME));
	ASSERT_EQUALS((unsigned)(4 * 3600), (unsigned)CSafeKad::MAX_BAN_TIME);

	// Past the ceiling the address is judged on its current behaviour alone.
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + CSafeKad::MAX_BAN_TIME + 1));
	ASSERT_EQUALS(0u, (unsigned)safe.GetBannedAddressCount());
	ASSERT_FALSE(safe.IsBadNode(
		IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, true, T0 + CSafeKad::MAX_BAN_TIME + 1));
}

TEST(SafeKad, ABannedAddressIsProblematicByConstruction)
{
	CSafeKad safe;
	safe.BanAddress(IP_A, T0);
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, T0));
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_B, T0));
	// And it is not tracked or re-marked while banned.
	safe.TrackProblematicNode(IP_A, PORT_A, T0);
	ASSERT_EQUALS(0u, (unsigned)safe.GetProblematicNodeCount());
	ASSERT_FALSE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));
	ASSERT_EQUALS(0u, (unsigned)safe.GetTrackedNodeCount());
}

TEST(SafeKad, TrackedTableStaysBoundedUnderSustainedTraffic)
{
	CSafeKad safe;
	// Every address is distinct and fresh, which is the shape of a flood:
	// nothing is old enough for the age horizon to reclaim.
	for (unsigned i = 0; i < CSafeKad::MAX_TRACKED_NODES + 500; ++i) {
		safe.TrackNode(0x20000000 + i, PORT_A, Id(i), true, T0);
	}
	ASSERT_TRUE(safe.GetTrackedNodeCount() <= CSafeKad::MAX_TRACKED_NODES);
	ASSERT_EQUALS((unsigned)CSafeKad::MAX_TRACKED_NODES, (unsigned)safe.GetTrackedNodeCount());
}

TEST(SafeKad, ProblematicTableStaysBoundedUnderSustainedTraffic)
{
	CSafeKad safe;
	for (unsigned i = 0; i < CSafeKad::MAX_PROBLEMATIC_NODES + 500; ++i) {
		safe.TrackProblematicNode(0x30000000 + i, PORT_A, T0);
	}
	ASSERT_EQUALS((unsigned)CSafeKad::MAX_PROBLEMATIC_NODES, (unsigned)safe.GetProblematicNodeCount());
}

TEST(SafeKad, BannedTableStaysBoundedUnderSustainedTraffic)
{
	CSafeKad safe;
	for (unsigned i = 0; i < CSafeKad::MAX_BANNED_ADDRESSES + 500; ++i) {
		safe.BanAddress(0x40000000 + i, T0);
	}
	ASSERT_EQUALS((unsigned)CSafeKad::MAX_BANNED_ADDRESSES, (unsigned)safe.GetBannedAddressCount());
}

TEST(SafeKad, TableCapacitiesMatchTheSpecifiedBounds)
{
	ASSERT_EQUALS(10000u, (unsigned)CSafeKad::MAX_TRACKED_NODES);
	ASSERT_EQUALS(10000u, (unsigned)CSafeKad::MAX_PROBLEMATIC_NODES);
	ASSERT_EQUALS(1000u, (unsigned)CSafeKad::MAX_BANNED_ADDRESSES);
	ASSERT_EQUALS(3600u, (unsigned)CSafeKad::MIN_ID_CHANGE_INTERVAL);
	ASSERT_EQUALS(300u, (unsigned)CSafeKad::MAX_PROBLEMATIC_TIME);
}

TEST(SafeKad, EvictionAtCapacityDropsTheLeastRecentlyReferencedEntry)
{
	CSafeKad safe;
	// One timestamp for the whole fill, so nothing is reclaimable by the age
	// horizon and the capacity path is what has to do the work.
	for (unsigned i = 0; i < CSafeKad::MAX_TRACKED_NODES; ++i) {
		safe.TrackNode(0x50000000 + i, PORT_A, Id(i), true, T0);
	}
	ASSERT_EQUALS((unsigned)CSafeKad::MAX_TRACKED_NODES, (unsigned)safe.GetTrackedNodeCount());

	// Refresh one address so it is strictly the most recently referenced.
	safe.TrackNode(0x50000005, PORT_A, Id(5), true, T0 + 1);

	// A new address has to displace something, and the table must not grow.
	ASSERT_TRUE(safe.TrackNode(0x5FFFFFFF, PORT_A, Id(0xFFFF), true, T0 + 1));
	ASSERT_EQUALS((unsigned)CSafeKad::MAX_TRACKED_NODES, (unsigned)safe.GetTrackedNodeCount());

	// The refreshed address survived: it still remembers its identity, so a
	// rotation inside the one-hour interval is refused.
	ASSERT_TRUE(safe.IsBadNode(0x50000005, PORT_A, Id(0xBEEF), KADEMLIA_VERSION, true, false, T0 + 1));

	// The least recently referenced address is the one that went: a new
	// identity for it is accepted as a first sighting rather than rejected as
	// a rotation, which is only possible if its entry is really gone.
	ASSERT_TRUE(safe.TrackNode(0x50000000, PORT_A, Id(0x1234), true, T0 + 1));
}

TEST(SafeKad, CleanupReclaimsEntriesPastTheirReferenceHorizon)
{
	CSafeKad safe;
	safe.TrackNode(IP_A, PORT_A, Id(1), true, T0);
	safe.TrackProblematicNode(IP_B, PORT_B, T0);
	safe.BanAddress(0x0A000003, T0);
	ASSERT_EQUALS(1u, (unsigned)safe.GetTrackedNodeCount());
	ASSERT_EQUALS(1u, (unsigned)safe.GetProblematicNodeCount());
	ASSERT_EQUALS(1u, (unsigned)safe.GetBannedAddressCount());

	// Nothing has referenced any of them for longer than every horizon.
	safe.Cleanup(T0 + CSafeKad::NODE_MAX_REFERENCE_AGE + 1);
	ASSERT_EQUALS(0u, (unsigned)safe.GetTrackedNodeCount());
	ASSERT_EQUALS(0u, (unsigned)safe.GetProblematicNodeCount());
	ASSERT_EQUALS(0u, (unsigned)safe.GetBannedAddressCount());
}

TEST(SafeKad, CleanupKeepsRecentlyReferencedEntries)
{
	CSafeKad safe;
	safe.TrackNode(IP_A, PORT_A, Id(1), true, T0);
	safe.Cleanup(T0 + CSafeKad::NODE_MAX_REFERENCE_AGE);
	ASSERT_EQUALS(1u, (unsigned)safe.GetTrackedNodeCount());
}

TEST(SafeKad, ClearEmptiesEveryTable)
{
	CSafeKad safe;
	safe.TrackNode(IP_A, PORT_A, Id(1), true, T0);
	safe.TrackProblematicNode(IP_B, PORT_B, T0);
	safe.BanAddress(0x0A000004, T0);

	safe.Clear();
	ASSERT_EQUALS(0u, (unsigned)safe.GetTrackedNodeCount());
	ASSERT_EQUALS(0u, (unsigned)safe.GetProblematicNodeCount());
	ASSERT_EQUALS(0u, (unsigned)safe.GetBannedAddressCount());
}

// An unverified identity change against a verified tracked entry is refused
// outright rather than rate-limited -- and refused without escalating.
//
// The refusal is what protects the entry: the identity we verified is still
// the one we hold, and the claim gets nothing. Escalating on top of it would
// be the mistake, because nothing about an unverified claim ties it to the
// address it names. A peer answering one of our Kad requests picks the
// (IP, port, ID) triples it lists, so the ladder would climb on somebody
// else's say-so and ban the node that did nothing.
TEST(SafeKad, AnUnverifiedIdentityChangeAgainstAVerifiedEntryIsRefusedNotEscalated)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));

	// Refused, and nothing recorded against the address.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), KADEMLIA_VERSION, false, false, T0 + 10));
	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, T0 + 10));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 10));

	// Retrying earns no more than the first attempt did. Free for the
	// sender, and that is the accepted cost: an unverified claim can waste
	// our time, but it must not be able to spend somebody else's reputation.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(3), KADEMLIA_VERSION, false, false, T0 + 20));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 20));

	// The entry survives intact, and the identity we verified still works.
	ASSERT_EQUALS(1u, (unsigned)safe.GetTrackedNodeCount());
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, false, T0 + 30));
}

// The same refusal for a pre-0x08 node, where it is the version rather than
// the verification state of the tracked entry that refuses the change. Such a
// node can never verify, so it can never be escalated either -- it keeps the
// refusal and loses nothing else.
TEST(SafeKad, AnUnverifiedPreVersion8IdentityChangeIsRefusedNotEscalated)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), false, T0));

	// Refused, both times, and never escalated. A pre-0x08 node cannot prove
	// which port it listens on, so nothing it says about its identity is
	// verified -- and an unverified claim is the one a third party can
	// fabricate. Refusing costs the sender its rotation; banning would let
	// anyone spoofing this address get the honest node behind it banned.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), 0x07, false, false, T0 + 10));
	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, T0 + 10));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 10));

	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(3), 0x07, false, false, T0 + 20));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 20));
}

// One rejected rotation is one step up the ladder, never two. Both refusal
// paths -- the outright one here and TrackNode's rate limit -- share a single
// escalation, so a single call must leave the address problematic and not
// banned. Double-counting would ban on first contact.
TEST(SafeKad, OneRejectedRotationEscalatesExactlyOneStep)
{
	CSafeKad safe;
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, false, T0));

	// Verified, so this goes through TrackNode's rate limit rather than the
	// outright refusal.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), KADEMLIA_VERSION, true, false, T0 + 10));
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, T0 + 10));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 10));
	ASSERT_EQUALS(1u, (unsigned)safe.GetProblematicNodeCount());

	CSafeKad other;
	ASSERT_FALSE(other.IsBadNode(IP_B, PORT_A, Id(1), KADEMLIA_VERSION, true, false, T0));
	// Unverified, so this takes the outright refusal -- which refuses without
	// escalating, and so leaves no problematic entry at all.
	ASSERT_TRUE(other.IsBadNode(IP_B, PORT_A, Id(2), KADEMLIA_VERSION, false, false, T0 + 10));
	ASSERT_FALSE(other.IsProblematic(IP_B, PORT_A, T0 + 10));
	ASSERT_FALSE(other.IsBanned(IP_B, T0 + 10));
	ASSERT_EQUALS(0u, (unsigned)other.GetProblematicNodeCount());
}

// The attack the verification requirement exists to stop, written out as a
// test because the code reads reasonable without it.
//
// ProcessKademlia2Response() calls AddUnfiltered() with verified hardcoded to
// false, and the peer answering our request chooses the (IP, port, ID) triples
// it lists. m_lastIDChange is stamped when an entry is created, so every
// freshly-learned contact sits inside the sub-hour window. Two fabricated
// mentions of an honest node would therefore have marked it problematic and
// then banned it for four hours, without that node ever sending us anything.
TEST(SafeKad, FabricatedUnverifiedMentionsCannotBanAnHonestNode)
{
	CSafeKad safe;

	// An honest node we have just learned about, exactly as a response
	// listing it would: unverified, and stamped now.
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, false, false, T0));

	// A hostile peer names the same address twice with identities it made
	// up, well inside the rotation window.
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), KADEMLIA_VERSION, false, false, T0 + 5));
	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(3), KADEMLIA_VERSION, false, false, T0 + 10));

	// Neither mention counted for anything.
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 10));
	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, T0 + 10));

	// And the honest node is still usable under the identity we hold.
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, false, false, T0 + 15));
}

// The other half: a peer that has proved which port it listens on and then
// cycles identities faster than once an hour is escalated exactly as before.
// Requiring verification narrows who can be banned, not what a ban is for.
TEST(SafeKad, AVerifiedRotationStillEscalatesToABan)
{
	CSafeKad safe;
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, false, T0));

	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(2), KADEMLIA_VERSION, true, false, T0 + 10));
	ASSERT_TRUE(safe.IsProblematic(IP_A, PORT_A, T0 + 10));
	ASSERT_FALSE(safe.IsBanned(IP_A, T0 + 10));

	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(3), KADEMLIA_VERSION, true, false, T0 + 20));
	ASSERT_TRUE(safe.IsBanned(IP_A, T0 + 20));
}

// A change refused only because it could not be verified, long past the
// interval, is not rapid rotation and must not escalate: a legacy client that
// legitimately reinstalled once a year is not a sybil, and banning it for four
// hours over a single refused change would be the protection misfiring.
TEST(SafeKad, ARefusedChangePastTheIntervalIsNotEscalated)
{
	CSafeKad safe;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, T0));

	// Keep the entry referenced while its last identity change ages, so the
	// reference horizon does not reclaim it before the interval elapses.
	// The refresh goes through TrackNode on purpose: IsBadNode() runs
	// Cleanup() before it looks the entry up, so refreshing through it would
	// reclaim the entry first and turn the next call into a first sighting.
	const time_t later = T0 + CSafeKad::MIN_ID_CHANGE_INTERVAL * 2;
	ASSERT_TRUE(safe.TrackNode(IP_A, PORT_A, Id(1), true, later - 1));

	ASSERT_TRUE(safe.IsBadNode(IP_A, PORT_A, Id(9), KADEMLIA_VERSION, false, false, later));
	ASSERT_FALSE(safe.IsProblematic(IP_A, PORT_A, later));
	ASSERT_FALSE(safe.IsBanned(IP_A, later));
	// The verified identity we hold is untouched and still usable.
	ASSERT_FALSE(safe.IsBadNode(IP_A, PORT_A, Id(1), KADEMLIA_VERSION, true, false, later));
}
