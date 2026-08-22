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
// Distinct peers that stay distinct as GUI_IDs, so a test asserting on
// session count is not accidentally asserting on hash collisions.
uint64 Peer(uint32 n)
{
	return GUI_ID(0xC0000000u + n, 4662);
}
} // namespace

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
	// The store-wide counter is what makes a single `since_id` cursor safe.
	// Per-session counters would let a client miss a message that landed in
	// another session while it was polling this one.
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
	// An outbound message carries no name. Letting it overwrite would leave
	// a session we already had a nick for rendering as a bare ip:port the
	// moment the user replied.
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
	// The 201st message drops the 1st; ids are NOT renumbered, because a
	// client holding a cursor into the evicted range must still advance past
	// it rather than re-reading.
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
	ASSERT_EQUALS(Peer(1), list[0]->gui_id);
	ASSERT_EQUALS(Peer(3), list[1]->gui_id);
	ASSERT_EQUALS(Peer(2), list[2]->gui_id);
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

TEST(ChatSessionStore, SessionCarriesTheDecodedIpAndPort)
{
	// The REST layer keys conversations on "<ip>:<port>", so the split has
	// to survive the GUI_ID round trip.
	CChatSessionStore store;
	store.AddIncoming(GUI_ID(0x0A000001u, 4662), "alice", "hi");
	const CChatSessionStore::Session *s = store.Find(GUI_ID(0x0A000001u, 4662));
	ASSERT_TRUE(s != nullptr);
	ASSERT_EQUALS(static_cast<uint32>(0x0A000001u), s->ip);
	ASSERT_EQUALS(static_cast<uint16>(4662), s->port);
}
