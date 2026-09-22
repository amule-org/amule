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

#include "ChatSessionStore.h"
#include "OtherFunctions.h" // GUI_ID

using namespace muleunit;

DECLARE_SIMPLE(ChatSessionStore)

namespace
{
CMD4Hash Peer(uint32 n)
{
	unsigned char bytes[16] = { 1 };
	for (unsigned i = 0; i < 4; ++i) {
		bytes[i + 1] = static_cast<unsigned char>(n >> (8 * i));
	}
	return CMD4Hash(bytes);
}

CNetworkAddress IPv6Route()
{
	CNetworkAddress::Octets bytes = { 0x20, 0x01, 0x48, 0x60 };
	bytes[15] = 1;
	return CNetworkAddress::IPv6FromOctets(bytes);
}
} // namespace

TEST(ChatSessionStore, ProvisionalOpenRequiresRouteAndReusesEndpoint)
{
	CChatSessionStore store;
	const auto route = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	ASSERT_TRUE(store.Open(CMD4Hash(), CNetworkAddress(), 4662, "").IsEmpty());
	ASSERT_TRUE(store.Open(CMD4Hash(), route, 0, "").IsEmpty());
	const auto first = store.Open(CMD4Hash(), route, 4662, "first");
	const auto second = store.Open(CMD4Hash(), route, 4662, "second");
	ASSERT_TRUE(first == second);
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
	ASSERT_TRUE(store.FindLegacy(GUI_ID(0x0100000Au, 4662)) == store.Find(first));
	ASSERT_EQUALS(static_cast<uint32>(1), store.AddOutgoing(first, "hello", route, 4662));
	ASSERT_EQUALS(static_cast<size_t>(1), store.Find(second)->messages.size());
	const CChatPeer independent(CMD4Hash(), route, 4662);
	ASSERT_TRUE(store.Promote(independent, Peer(1)));
	ASSERT_TRUE(first.Hash() == Peer(1));
	ASSERT_TRUE(independent == first);
}

TEST(ChatSessionStore, PromotionPreservesCopiedHandleAndReplacementTranscript)
{
	CChatSessionStore store;
	const auto route = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	const auto provisional = store.Open(CMD4Hash(), route, 4662, "alice");
	const auto queuedNotification = provisional;
	store.AddOutgoing(provisional, "before", route, 4662);
	store.AddIncoming(Peer(7), "alice", "identified", route, 4662);
	ASSERT_TRUE(store.Promote(provisional, Peer(7)));
	ASSERT_TRUE(queuedNotification.Hash() == Peer(7));
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
	store.AddOutgoing(CChatPeer(Peer(7)), "replacement", route, 4662);
	const auto *session = store.Find(queuedNotification);
	ASSERT_EQUALS(static_cast<size_t>(3), session->messages.size());
	ASSERT_EQUALS(static_cast<uint32>(1), session->messages[0].id);
	ASSERT_EQUALS(static_cast<uint32>(2), session->messages[1].id);
	ASSERT_EQUALS(static_cast<uint32>(3), session->messages[2].id);
	ASSERT_TRUE(!store.Promote(provisional, Peer(8)));
	ASSERT_TRUE(store.Open(Peer(7), route, 4662, "alice") == provisional);
	ASSERT_TRUE(store.CloseSession(queuedNotification));
	ASSERT_TRUE(store.Find(Peer(7)) == nullptr);
}

TEST(ChatSessionStore, PromotionDoesNotMergeOtherProvisionalOccupants)
{
	CChatSessionStore store;
	const auto route = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	const auto first = store.Open(CMD4Hash(), route, 4662, "first");
	ASSERT_TRUE(store.Promote(first, Peer(1)));
	const auto second = store.Open(CMD4Hash(), route, 4662, "second");
	ASSERT_TRUE(second.Hash().IsEmpty());
	ASSERT_EQUALS(static_cast<size_t>(2), store.SessionCount());
	ASSERT_TRUE(store.FindLegacy(GUI_ID(0x0100000Au, 4662)) == nullptr);
	ASSERT_TRUE(store.Promote(second, Peer(2)));
	ASSERT_TRUE(first != second);
}

TEST(ChatSessionStore, ProvisionalRoutesNormalizeMappedIPv4AndKeepPortsDistinct)
{
	CChatSessionStore store;
	const auto route = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	CNetworkAddress::Octets mapped = {};
	mapped[10] = mapped[11] = 0xff;
	mapped[12] = 10;
	mapped[15] = 1;
	const auto first = store.Open(CMD4Hash(), route, 4662, "alice");
	const auto normalized =
		store.Open(CMD4Hash(), CNetworkAddress::IPv6FromOctets(mapped), 4662, "alice");
	ASSERT_TRUE(first == normalized);
	ASSERT_TRUE(first != store.Open(CMD4Hash(), route, 4663, "other port"));
	ASSERT_TRUE(first != store.Open(CMD4Hash(), IPv6Route(), 4662, "v6"));
	ASSERT_EQUALS(static_cast<size_t>(3), store.SessionCount());
	ASSERT_TRUE(!store.Promote(CChatPeer(CMD4Hash(), route, 9999), Peer(1)));
}

TEST(ChatSessionStore, IndependentPromotionMergesInterleavedHistoryInCursorOrder)
{
	CChatSessionStore store;
	const auto route = IPv6Route();
	const auto provisional = store.Open(CMD4Hash(), route, 4662, "alice");
	store.AddOutgoing(provisional, "one", route, 4662);
	store.AddIncoming(Peer(7), "alice", "two", route, 4662);
	store.AddOutgoing(provisional, "three", route, 4662);
	const CChatPeer independent(CMD4Hash(), route, 4662);
	ASSERT_TRUE(store.Promote(independent, Peer(7)));
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
	const auto *session = store.Find(Peer(7));
	ASSERT_EQUALS(static_cast<size_t>(3), session->messages.size());
	for (size_t i = 0; i < 3; ++i) {
		ASSERT_EQUALS(static_cast<uint32>(i + 1), session->messages[i].id);
	}
	ASSERT_TRUE(provisional == independent);
	ASSERT_TRUE(!store.Promote(independent, Peer(8)));
}

TEST(ChatSessionStore, StartsEmpty)
{
	CChatSessionStore store;
	ASSERT_EQUALS(static_cast<size_t>(0), store.SessionCount());
	ASSERT_EQUALS(static_cast<uint32>(0), store.LastMsgId());
	ASSERT_TRUE(store.Find(Peer(1)) == nullptr);
}

TEST(ChatSessionStore, FirstMessageCreatesSessionWithIdOne)
{
	// id 0 is the "no cursor yet" sentinel every reader uses, so the first
	// real message must be 1 or a client starting at 0 would skip it.
	CChatSessionStore store;
	const uint32 id = store.AddIncoming(Peer(1), "alice", "hi");
	ASSERT_EQUALS(static_cast<uint32>(1), id);
	ASSERT_EQUALS(static_cast<uint32>(1), store.LastMsgId());
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());

	const CChatSessionStore::Session *s = store.Find(Peer(1));
	ASSERT_TRUE(s != nullptr);
	ASSERT_EQUALS(wxString("alice"), s->name);
	ASSERT_EQUALS(static_cast<size_t>(1), s->messages.size());
	ASSERT_EQUALS(wxString("hi"), s->messages[0].text);
	ASSERT_EQUALS(static_cast<uint8>(CChatSessionStore::DIR_IN), s->messages[0].direction);
}

TEST(ChatSessionStore, IdsAreMonotonicAcrossSessions)
{
	// The store-wide counter is what makes a single `since_id` cursor safe. Per-session
	// counters would let a client miss a message that landed in another session while it was
	// polling this one.
	CChatSessionStore store;
	ASSERT_EQUALS(static_cast<uint32>(1), store.AddIncoming(Peer(1), "alice", "a"));
	ASSERT_EQUALS(static_cast<uint32>(2), store.AddIncoming(Peer(2), "bob", "b"));
	ASSERT_EQUALS(static_cast<uint32>(3), store.AddOutgoing(Peer(1), "c"));
	ASSERT_EQUALS(static_cast<uint32>(3), store.LastMsgId());
	ASSERT_EQUALS(static_cast<uint32>(3), store.Find(Peer(1))->LastMsgId());
	ASSERT_EQUALS(static_cast<uint32>(2), store.Find(Peer(2))->LastMsgId());
}

TEST(ChatSessionStore, OutgoingDoesNotEraseAKnownPeerName)
{
	// An outbound message carries no name. Letting it overwrite would leave a session we
	// already had a nick for rendering as a bare ip:port the moment the user replied.
	CChatSessionStore store;
	store.AddIncoming(Peer(1), "alice", "hi");
	store.AddOutgoing(Peer(1), "hello back");
	ASSERT_EQUALS(wxString("alice"), store.Find(Peer(1))->name);
}

TEST(ChatSessionStore, LaterNameFillsInAnEmptyOne)
{
	// The reverse case: a peer whose nick was unknown on the first message
	// must pick it up when a later one carries it.
	CChatSessionStore store;
	store.AddIncoming(Peer(1), wxEmptyString, "hi");
	ASSERT_TRUE(store.Find(Peer(1))->name.IsEmpty());
	store.AddIncoming(Peer(1), "alice", "again");
	ASSERT_EQUALS(wxString("alice"), store.Find(Peer(1))->name);
}

TEST(ChatSessionStore, MessageCapEvictsOldestKeepingIdsIntact)
{
	// The 201st message drops the 1st; ids are NOT renumbered, because a client holding a
	// cursor into the evicted range must still advance past it rather than re-reading.
	CChatSessionStore store;
	const size_t cap = CChatSessionStore::MAX_MESSAGES_PER_SESSION;
	for (size_t i = 0; i < cap; ++i) {
		store.AddIncoming(Peer(1), "alice", wxString::Format("m%zu", i));
	}
	const CChatSessionStore::Session *s = store.Find(Peer(1));
	ASSERT_EQUALS(cap, s->messages.size());
	ASSERT_EQUALS(wxString("m0"), s->messages.front().text);
	ASSERT_EQUALS(static_cast<uint32>(1), s->messages.front().id);

	store.AddIncoming(Peer(1), "alice", "overflow");
	s = store.Find(Peer(1));
	ASSERT_EQUALS(cap, s->messages.size());
	ASSERT_EQUALS(wxString("m1"), s->messages.front().text);
	ASSERT_EQUALS(static_cast<uint32>(2), s->messages.front().id);
	ASSERT_EQUALS(wxString("overflow"), s->messages.back().text);
	ASSERT_EQUALS(static_cast<uint32>(cap + 1), s->messages.back().id);
}

TEST(ChatSessionStore, SessionCapEvictsLeastRecentlyActive)
{
	CChatSessionStore store;
	const size_t cap = CChatSessionStore::MAX_SESSIONS;
	for (size_t i = 0; i < cap; ++i) {
		store.AddIncoming(Peer(static_cast<uint32>(i)), "peer", "hi");
	}
	ASSERT_EQUALS(cap, store.SessionCount());

	// Peer(0) is the least recently active, so it is the one to go.
	store.AddIncoming(Peer(9999), "newcomer", "hi");
	ASSERT_EQUALS(cap, store.SessionCount());
	ASSERT_TRUE(store.Find(Peer(0)) == nullptr);
	ASSERT_TRUE(store.Find(Peer(9999)) != nullptr);
	ASSERT_TRUE(store.Find(Peer(1)) != nullptr);
}

TEST(ChatSessionStore, ActivityRefreshRescuesASessionFromEviction)
{
	// Eviction is by activity, not by creation order: a long-running
	// conversation must not be dropped just because it started first.
	CChatSessionStore store;
	const size_t cap = CChatSessionStore::MAX_SESSIONS;
	for (size_t i = 0; i < cap; ++i) {
		store.AddIncoming(Peer(static_cast<uint32>(i)), "peer", "hi");
	}
	store.AddOutgoing(Peer(0), "still talking"); // moves Peer(0) to the front
	store.AddIncoming(Peer(9999), "newcomer", "hi");

	ASSERT_TRUE(store.Find(Peer(0)) != nullptr);
	ASSERT_TRUE(store.Find(Peer(1)) == nullptr); // now the least recently active
}

TEST(ChatSessionStore, SessionsAreMostRecentlyActiveFirst)
{
	CChatSessionStore store;
	store.AddIncoming(Peer(1), "alice", "a");
	store.AddIncoming(Peer(2), "bob", "b");
	store.AddIncoming(Peer(3), "carol", "c");
	store.AddOutgoing(Peer(1), "reply to alice");

	std::vector<const CChatSessionStore::Session *> list = store.Sessions();
	ASSERT_EQUALS(static_cast<size_t>(3), list.size());
	ASSERT_TRUE(Peer(1) == list[0]->peer);
	ASSERT_TRUE(Peer(3) == list[1]->peer);
	ASSERT_TRUE(Peer(2) == list[2]->peer);
}

TEST(ChatSessionStore, CloseRemovesOnlyThatSession)
{
	CChatSessionStore store;
	store.AddIncoming(Peer(1), "alice", "a");
	store.AddIncoming(Peer(2), "bob", "b");

	ASSERT_TRUE(store.CloseSession(Peer(1)));
	ASSERT_TRUE(store.Find(Peer(1)) == nullptr);
	ASSERT_TRUE(store.Find(Peer(2)) != nullptr);
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
}

TEST(ChatSessionStore, CloseOfUnknownSessionReportsFailure)
{
	// The EC handler answers "no such session" off this return rather than
	// silently succeeding, so an unknown id must be distinguishable.
	CChatSessionStore store;
	ASSERT_TRUE(!store.CloseSession(Peer(1)));
}

TEST(ChatSessionStore, IdsKeepAdvancingAfterAClose)
{
	// Reopening a closed conversation must not reissue ids a client already
	// holds: the counter is store-wide and never rewinds.
	CChatSessionStore store;
	store.AddIncoming(Peer(1), "alice", "a");
	store.AddIncoming(Peer(1), "alice", "b");
	store.CloseSession(Peer(1));
	ASSERT_EQUALS(static_cast<uint32>(3), store.AddIncoming(Peer(1), "alice", "c"));
	ASSERT_EQUALS(static_cast<size_t>(1), store.Find(Peer(1))->messages.size());
}

TEST(ChatSessionStore, RouteChangesFromIPv4ToIPv6WithoutChangingIdentity)
{
	CChatSessionStore store;
	const auto v4 = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	store.AddIncoming(Peer(1), "alice", "hi", v4, 4662);
	const uint64 legacy = GUI_ID(0x0100000Au, 4662);
	ASSERT_TRUE(store.FindLegacy(legacy) == store.Find(Peer(1)));
	store.AddOutgoing(Peer(1), "reply", IPv6Route(), 4663);
	const auto *s = store.Find(Peer(1));
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
	ASSERT_EQUALS(static_cast<size_t>(2), s->messages.size());
	ASSERT_TRUE(s->address == IPv6Route());
	ASSERT_EQUALS(static_cast<uint16>(4663), s->port);
	ASSERT_EQUALS(static_cast<uint64>(0), s->LegacyGuiId());
	ASSERT_TRUE(store.FindLegacy(legacy) == nullptr);
}

TEST(ChatSessionStore, ReplacementClientWithSameHashContinuesTranscript)
{
	// The store boundary deliberately accepts no object pointer or ECID. These
	// independently reconstructed hash values model two successive client objects.
	CChatSessionStore store;
	{
		const CMD4Hash firstClientHash = Peer(7);
		store.AddIncoming(firstClientHash, "alice", "before replacement");
	}
	const CMD4Hash replacementClientHash = Peer(7);
	store.AddOutgoing(replacementClientHash, "after replacement", IPv6Route(), 4662);
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
	ASSERT_EQUALS(static_cast<size_t>(2), store.Find(replacementClientHash)->messages.size());
}

TEST(ChatSessionStore, DistinctPeersAtSameEndpointAreNeverMerged)
{
	CChatSessionStore store;
	const auto route = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	store.AddIncoming(Peer(1), "alice", "a", route, 4662);
	store.AddIncoming(Peer(2), "bob", "b", route, 4662);
	ASSERT_EQUALS(static_cast<size_t>(2), store.SessionCount());
	ASSERT_EQUALS(wxString("a"), store.Find(Peer(1))->messages.front().text);
	ASSERT_EQUALS(wxString("b"), store.Find(Peer(2))->messages.front().text);
	ASSERT_TRUE(store.FindLegacy(GUI_ID(0x0100000Au, 4662)) == nullptr);
	store.CloseSession(Peer(1));
	ASSERT_TRUE(store.FindLegacy(GUI_ID(0x0100000Au, 4662)) == store.Find(Peer(2)));
}

TEST(ChatSessionStore, AbsentLowIDRouteDoesNotBecomeAnIdentity)
{
	// A callback-only LowID is not an IPv4 address. Its route can be absent,
	// while its hash still identifies a conversation and keeps peers separate.
	CChatSessionStore store;
	store.AddIncoming(Peer(1), "alice", "a", CNetworkAddress::Absent(), 4662);
	store.AddIncoming(Peer(2), "bob", "b", CNetworkAddress::Absent(), 4662);
	ASSERT_EQUALS(static_cast<size_t>(2), store.SessionCount());
	ASSERT_TRUE(store.Find(Peer(1))->address.IsAbsent());
	ASSERT_EQUALS(static_cast<uint64>(0), store.Find(Peer(1))->LegacyGuiId());
	ASSERT_TRUE(store.FindLegacy(0) == nullptr);
	store.AddOutgoing(Peer(1), "route acquired", IPv6Route(), 4662);
	ASSERT_EQUALS(static_cast<size_t>(2), store.Find(Peer(1))->messages.size());
}

TEST(ChatSessionStore, EmptyHashesAreUnavailableAndDoNotConsumeMessageIds)
{
	CChatSessionStore store;
	ASSERT_EQUALS(static_cast<uint32>(0), store.AddIncoming(CMD4Hash(), "unknown", "hi"));
	ASSERT_EQUALS(static_cast<uint32>(0), store.AddOutgoing(CMD4Hash(), "reply", IPv6Route(), 4662));
	ASSERT_EQUALS(static_cast<size_t>(0), store.SessionCount());
	ASSERT_EQUALS(static_cast<uint32>(0), store.LastMsgId());
	ASSERT_TRUE(!store.CloseSession(CMD4Hash()));
	ASSERT_EQUALS(static_cast<uint32>(1), store.AddIncoming(Peer(1), "known", "hi"));
}

TEST(ChatSessionStore, LegacyProjectionRequiresIPv4AddressAndPort)
{
	CChatSessionStore store;
	const auto route = CNetworkAddress::FromIPv4NetworkOrder(0x0100000Au);
	store.AddIncoming(Peer(1), "alice", "hi", route, 4662);
	ASSERT_EQUALS(GUI_ID(0x0100000Au, 4662), store.Find(Peer(1))->LegacyGuiId());
	store.AddOutgoing(Peer(1), "no port", route, 0);
	ASSERT_EQUALS(static_cast<uint64>(0), store.Find(Peer(1))->LegacyGuiId());
	store.AddOutgoing(Peer(1), "no address", CNetworkAddress::FromIPv4NetworkOrder(0), 4662);
	ASSERT_EQUALS(static_cast<uint64>(0), store.Find(Peer(1))->LegacyGuiId());
	store.AddOutgoing(Peer(1), "v6", IPv6Route(), 4662);
	ASSERT_EQUALS(static_cast<uint64>(0), store.Find(Peer(1))->LegacyGuiId());
	CNetworkAddress::Octets mapped = {};
	mapped[10] = mapped[11] = 0xff;
	mapped[12] = 10;
	mapped[15] = 1;
	store.AddOutgoing(Peer(1), "mapped", CNetworkAddress::IPv6FromOctets(mapped), 4662);
	ASSERT_EQUALS(static_cast<uint64>(0), store.Find(Peer(1))->LegacyGuiId());
	store.AddOutgoing(Peer(1), "route lost", CNetworkAddress::Absent(), 0);
	ASSERT_TRUE(store.Find(Peer(1))->address.IsAbsent());
	ASSERT_EQUALS(static_cast<size_t>(1), store.SessionCount());
	store.AddOutgoing(Peer(1), "v4 restored", route, 4662);
	ASSERT_TRUE(store.FindLegacy(GUI_ID(0x0100000Au, 4662)) == store.Find(Peer(1)));
}
