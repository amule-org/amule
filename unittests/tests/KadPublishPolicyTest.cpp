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

// How we publish ourselves as a Kad source. A node firewalled on IPv4 with neither a direct UDP
// callback nor a buddy published nothing, although peers that hold an IPv6 route could reach it at
// the IPv6 address it advertises: aMule since #1817 and eMuleAI add a firewalled source by its "ip6"
// tag alone.

#include <muleunit/test.h>

#include "KadPublishPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadPublishPolicy)

namespace
{
SKadPublishFacts Firewalled()
{
	SKadPublishFacts facts;
	facts.firewalled = true;
	return facts;
}
} // namespace

TEST(KadPublishPolicy, AnOpenNodePublishesAsHighID)
{
	SKadPublishFacts open;
	open.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(open) == EKadSourcePublish::HighID);
}

// The IPv4 callback routes keep their precedence: they reach peers with no IPv6 route as well, and
// the "ip6" tag rides along with them anyway.
TEST(KadPublishPolicy, AnIPv4CallbackRouteIsPreferredOverIPv6Alone)
{
	SKadPublishFacts direct = Firewalled();
	direct.directCallback = true;
	direct.hasBuddy = true;
	direct.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(direct) == EKadSourcePublish::DirectCallback);

	SKadPublishFacts buddy = Firewalled();
	buddy.hasBuddy = true;
	buddy.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(buddy) == EKadSourcePublish::Buddy);
}

// #1926: a direct UDP callback is a route back on its own, with no buddy and no IPv6.
TEST(KadPublishPolicy, AFirewalledNodeWithOnlyADirectCallbackPublishes)
{
	SKadPublishFacts facts = Firewalled();
	facts.directCallback = true;
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::DirectCallback);
	ASSERT_TRUE(PublishesToKad(facts));
}

TEST(KadPublishPolicy, OnlyANodeWithNoRouteBackStaysOffKad)
{
	SKadPublishFacts open;
	ASSERT_TRUE(PublishesToKad(open));

	SKadPublishFacts buddy = Firewalled();
	buddy.hasBuddy = true;
	ASSERT_TRUE(PublishesToKad(buddy));

	SKadPublishFacts ipv6 = Firewalled();
	ipv6.advertisesIPv6 = true;
	ASSERT_TRUE(PublishesToKad(ipv6));

	ASSERT_FALSE(PublishesToKad(Firewalled()));
}

// The case this adds: no IPv4 route back to us, but an IPv6 address we advertise.
TEST(KadPublishPolicy, AFirewalledNodeWithoutCallbackPublishesOnItsIPv6)
{
	SKadPublishFacts facts = Firewalled();
	facts.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::IPv6Only);
}

// Without any route back there is nothing to publish, as before.
TEST(KadPublishPolicy, AFirewalledNodeWithNoRouteBackPublishesNothing)
{
	const SKadPublishFacts facts = Firewalled();
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::None);
}

// #2513: with no buddy and no IPv6, type 7 with the pointer route keeps our records reachable.
TEST(KadPublishPolicy, AFirewalledNodeWithOnlyThePointerPublishesTypeSeven)
{
	SKadPublishFacts facts = Firewalled();
	facts.pointerEnabled = true;
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::PointerOnly);
	ASSERT_EQUALS(7, KadSourceTypeFor(EKadSourcePublish::PointerOnly, false));
	ASSERT_EQUALS(7, KadSourceTypeFor(EKadSourcePublish::PointerOnly, true));
	ASSERT_TRUE(PublishesToKad(facts));

	// A buddy or an IPv6 route comes first.
	facts.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::IPv6Only);
	facts.hasBuddy = true;
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::Buddy);
}

// The pointer alone keeps source records live; it gives no reason to publish keywords and notes.
TEST(KadPublishPolicy, ThePointerAlonePublishesNoKeywords)
{
	SKadPublishFacts pointer = Firewalled();
	pointer.pointerEnabled = true;
	ASSERT_FALSE(PublishesKadKeywords(pointer));
	ASSERT_FALSE(PublishesKadKeywords(Firewalled()));

	SKadPublishFacts ipv6 = Firewalled();
	ipv6.advertisesIPv6 = true;
	ASSERT_TRUE(PublishesKadKeywords(ipv6));
	SKadPublishFacts buddy = Firewalled();
	buddy.hasBuddy = true;
	ASSERT_TRUE(PublishesKadKeywords(buddy));
}

TEST(KadPublishPolicy, TypeSevenCarriesThePointerRouteWhenEnabled)
{
	ASSERT_TRUE(Type7CarriesKadPointer(EKadSourcePublish::PointerOnly, true));
	ASSERT_TRUE(Type7CarriesKadPointer(EKadSourcePublish::IPv6Only, true));
	ASSERT_FALSE(Type7CarriesKadPointer(EKadSourcePublish::IPv6Only, false));
	ASSERT_FALSE(Type7CarriesKadPointer(EKadSourcePublish::Buddy, true));
}

// Type 7 only fills: an entry with the marker still leads to us through the pointer.
TEST(KadPublishPolicy, TypeSevenLeavesALiveMarkedEntryAlone)
{
	const uint32_t now = 100000;
	const uint32_t lifetime = 18000;
	ASSERT_FALSE(IsKadSourceFillDue(now - 60, true, now, lifetime));
	ASSERT_TRUE(IsKadSourceFillDue(now - 60, false, now, lifetime));      // Published without it.
	ASSERT_TRUE(IsKadSourceFillDue(now - lifetime, true, now, lifetime)); // Expired.
	ASSERT_TRUE(IsKadSourceFillDue(0, false, now, lifetime));             // Never published.
	ASSERT_TRUE(IsKadSourceFillDue(now + 60, true, now, lifetime));       // Clock stepped back.
}

TEST(KadPublishPolicy, AnAcknowledgementLivesLessThanItsLifetime)
{
	const uint32_t now = 1000000;
	const uint32_t lifetime = 5 * 3600;
	ASSERT_TRUE(IsKadAckLive(now, now, lifetime));
	ASSERT_TRUE(IsKadAckLive(now - lifetime + 1, now, lifetime));
	ASSERT_FALSE(IsKadAckLive(now - lifetime, now, lifetime)); // Expired.
	ASSERT_FALSE(IsKadAckLive(0, now, lifetime));              // None.
	ASSERT_FALSE(IsKadAckLive(now + 60, now, lifetime));       // Clock stepped back.
}

TEST(KadPublishPolicy, ALookupAnswerFillsTheMissingKeyOfAContactTheSearchOwns)
{
	ASSERT_TRUE(KeepsLookupResponderKey(true, 6, 0, 0x1234));
}

// A key on a routing table contact gates its updates, so a spoofed answer must not set one.
TEST(KadPublishPolicy, ALookupAnswerNeverSetsTheKeyOfARoutingTableContact)
{
	ASSERT_FALSE(KeepsLookupResponderKey(false, 6, 0, 0x1234));
}

TEST(KadPublishPolicy, ALaterLookupAnswerDoesNotReplaceAKey)
{
	ASSERT_FALSE(KeepsLookupResponderKey(true, 6, 0x1111, 0x1234));
}

TEST(KadPublishPolicy, AnAnswerWithoutAKeyStoresNothing)
{
	ASSERT_FALSE(KeepsLookupResponderKey(true, 6, 0, 0));
}

// Packets to these contacts go out with no key, whatever the contact claims to have sent.
TEST(KadPublishPolicy, AContactBelowKadVersionSixKeepsNoKey)
{
	ASSERT_FALSE(KeepsLookupResponderKey(true, 5, 0, 0x1234));
}

TEST(KadPublishPolicy, AnAcknowledgedEntryLivesForTheLifetimeNodesKeepIt)
{
	const uint32_t now = 100000;
	const uint32_t lifetime = 18000;
	const auto live = [&](uint32_t ackTime) {
		return KadSourceEntriesLive(EKadSourcePublish::HighID, ackTime, 0, 0, now, lifetime, 0, 0);
	};
	ASSERT_TRUE(live(now - 60));
	ASSERT_TRUE(live(now - lifetime + 1));
	ASSERT_FALSE(live(now - lifetime)); // Expired on the nodes.
	ASSERT_FALSE(live(0));              // Never acknowledged.
	ASSERT_FALSE(live(now + 60));       // A clock step back must not count it twice.
}

TEST(KadPublishPolicy, AnEntryMustNameTheBuddyWePublishWithNow)
{
	const uint32_t now = 100000;
	const uint32_t lifetime = 18000;
	const uint32_t ack = now - 60;
	// Published through buddy 1:10, still our buddy.
	ASSERT_TRUE(KadSourceEntriesLive(EKadSourcePublish::Buddy, ack, 1, 10, now, lifetime, 1, 10));
	// The buddy changed: callbacks for the old entry go to a node that no longer relays for us.
	ASSERT_FALSE(KadSourceEntriesLive(EKadSourcePublish::Buddy, ack, 1, 10, now, lifetime, 2, 10));
	ASSERT_FALSE(KadSourceEntriesLive(EKadSourcePublish::Buddy, ack, 1, 10, now, lifetime, 1, 11));
	// Became HighID: an entry naming a buddy no longer describes how to reach us.
	ASSERT_FALSE(KadSourceEntriesLive(EKadSourcePublish::HighID, ack, 1, 10, now, lifetime, 0, 0));
	// With no route back at all, nothing we published leads to us.
	ASSERT_FALSE(KadSourceEntriesLive(EKadSourcePublish::None, ack, 0, 0, now, lifetime, 0, 0));
}

TEST(KadPublishPolicy, EachPublishKindCarriesItsOwnSourceType)
{
	ASSERT_EQUALS(1, KadSourceTypeFor(EKadSourcePublish::HighID, false));
	ASSERT_EQUALS(4, KadSourceTypeFor(EKadSourcePublish::HighID, true));
	ASSERT_EQUALS(6, KadSourceTypeFor(EKadSourcePublish::DirectCallback, false));
	ASSERT_EQUALS(6, KadSourceTypeFor(EKadSourcePublish::DirectCallback, true));
	ASSERT_EQUALS(3, KadSourceTypeFor(EKadSourcePublish::Buddy, false));
	ASSERT_EQUALS(5, KadSourceTypeFor(EKadSourcePublish::Buddy, true));
}

// Type 7 says "reach me by one of the routes in my tags", so one value covers every file size.
TEST(KadPublishPolicy, AnIPv6OnlySourceIsTypeSevenWhateverTheFileSize)
{
	ASSERT_EQUALS(7, KadSourceTypeFor(EKadSourcePublish::IPv6Only, false));
	ASSERT_EQUALS(7, KadSourceTypeFor(EKadSourcePublish::IPv6Only, true));
}

TEST(KadPublishPolicy, NoRouteBackHasNoSourceTypeToPublish)
{
	ASSERT_EQUALS(0, KadSourceTypeFor(EKadSourcePublish::None, false));
	ASSERT_EQUALS(0, KadSourceTypeFor(EKadSourcePublish::None, true));
}

TEST(KadPublishPolicy, AKeywordNodeAnswersOncePerFiftyFiles)
{
	ASSERT_EQUALS(7u, AnsweringNodes(7, 0));
	ASSERT_EQUALS(7u, AnsweringNodes(7, 1));
	ASSERT_EQUALS(7u, AnsweringNodes(7, 50));
	ASSERT_EQUALS(7u, AnsweringNodes(14, 51));
	ASSERT_EQUALS(7u, AnsweringNodes(21, 150));
}

// The publisher adds a 151st file ID before it stops, but a node is sent at most 150 files.
TEST(KadPublishPolicy, AKeywordNodeIsSentAtMostThreePackets)
{
	ASSERT_EQUALS(7u, AnsweringNodes(21, 151));
	ASSERT_EQUALS(7u, AnsweringNodes(21, 1000));
}

// A firewalled record declares the NAT-T transports we serve, so a firewalled requester can dial
// us over uTP or QUIC (eMuleAI over either, eMuleQt over QUIC). A HighID record is reached over
// TCP, and type 7 names no IPv4 route a transport could use.
TEST(KadPublishPolicy, OnlyAFirewalledIPv4RecordDeclaresOurNatTTransports)
{
	const uint8_t crypt = 0x0B; // crypt supported, requested, and the direct callback bit
	const uint8_t natT = 0x80 | 0x40;
	ASSERT_EQUALS(static_cast<uint8_t>(crypt | natT),
		KadSourceConnectOptions(EKadSourcePublish::DirectCallback, crypt, natT));
	ASSERT_EQUALS(static_cast<uint8_t>(crypt | 0x80),
		KadSourceConnectOptions(EKadSourcePublish::Buddy, crypt, 0x80));
	ASSERT_EQUALS(crypt, KadSourceConnectOptions(EKadSourcePublish::HighID, crypt, natT));
	ASSERT_EQUALS(crypt, KadSourceConnectOptions(EKadSourcePublish::IPv6Only, crypt, natT));
	ASSERT_EQUALS(crypt, KadSourceConnectOptions(EKadSourcePublish::PointerOnly, crypt, natT));
	// Only the two transport bits are taken from what we serve.
	ASSERT_EQUALS(crypt, KadSourceConnectOptions(EKadSourcePublish::DirectCallback, crypt, 0x3F));
}

// File_checked_for_headers
