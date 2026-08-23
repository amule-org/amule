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

#include <BrowseStore.h>

using namespace muleunit;
using namespace browse;

DECLARE_SIMPLE(BrowseStore)

namespace
{
// Stand-ins for peers. Only their addresses matter -- the store never
// dereferences a client, which is what lets these be anything at all.
int g_alice = 0;
int g_bob = 0;
Store::ClientKey ALICE = &g_alice;
Store::ClientKey BOB = &g_bob;

constexpr std::uint32_t kSidA = 10;
constexpr std::uint32_t kSidB = 20;
} // namespace

// --- Starting, and the join the EC handler depends on. ----------------

TEST(BrowseStore, StartTracksTheBrowseAndAnswersForThePeer)
{
	Store s;
	ASSERT_TRUE(s.Start(ALICE, kSidA, 7, 1000).started);
	ASSERT_EQUALS(kSidA, s.SearchIdFor(ALICE));
	ASSERT_TRUE(s.Has(kSidA));
	ASSERT_TRUE(s.StateOf(kSidA) == State::InProgress);
	ASSERT_EQUALS(static_cast<std::size_t>(1), s.Size());
}

TEST(BrowseStore, ASecondBrowseOfTheSamePeerIsRefused)
{
	// This is the rule the EC handler's "join the browse already in flight"
	// rests on: there is one exchange with a peer, so a second record could
	// only ever describe the same one.
	Store s;
	ASSERT_TRUE(s.Start(ALICE, kSidA, 7, 1000).started);
	ASSERT_FALSE(s.Start(ALICE, kSidB, 7, 1000).started);
	ASSERT_EQUALS(static_cast<std::size_t>(1), s.Size());
	ASSERT_EQUALS(kSidA, s.SearchIdFor(ALICE));
}

TEST(BrowseStore, DifferentPeersAreTrackedIndependently)
{
	Store s;
	ASSERT_TRUE(s.Start(ALICE, kSidA, 7, 1000).started);
	ASSERT_TRUE(s.Start(BOB, kSidB, 8, 1000).started);
	ASSERT_EQUALS(kSidA, s.SearchIdFor(ALICE));
	ASSERT_EQUALS(kSidB, s.SearchIdFor(BOB));

	s.Fail(ALICE);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
	ASSERT_TRUE(s.StateOf(kSidB) == State::InProgress);
}

TEST(BrowseStore, StartRejectsNonsense)
{
	Store s;
	ASSERT_FALSE(s.Start(nullptr, kSidA, 7, 1000).started);
	ASSERT_FALSE(s.Start(ALICE, 0, 7, 1000).started);
	ASSERT_EQUALS(static_cast<std::size_t>(0), s.Size());
}

TEST(BrowseStore, AFinishedBrowseNoLongerAnswersAsThePeersLiveOne)
{
	// SearchIdFor reports only a browse in progress, so a peer whose browse
	// has ended can be browsed afresh rather than joined to a dead one.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Finish(ALICE);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchIdFor(ALICE));
	// ...but it is still on the books, and still answers for its state.
	ASSERT_TRUE(s.Has(kSidA));
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
}

// --- The two bugs that live verification caught, not the tests. --------

TEST(BrowseStore, ATerminalRecordOutlivesItsClient)
{
	// Erasing the record when the browse ended was a real regression: every
	// consumer asks the store what state a search is in, so a dropped record
	// sends the listing back to guessing from whether results were retained --
	// which reports a failed browse as idle, forever.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_TRUE(s.Fail(ALICE).effect == Effect::AnnounceFailure);

	// The tick that follows releases the peer and nothing else.
	const auto todo = s.Tick(2000);
	ASSERT_EQUALS(static_cast<std::size_t>(1), todo.size());
	ASSERT_EQUALS(kSidA, todo[0].searchId);
	ASSERT_TRUE(todo[0].effect == Effect::ReleaseClient);

	ASSERT_TRUE(s.Has(kSidA));
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);

	// And it settles: no further ticks keep reporting it.
	ASSERT_TRUE(s.Tick(3000).empty());
	ASSERT_TRUE(s.Tick(999999).empty());
}

TEST(BrowseStore, ADisconnectAfterSuccessIsNotReportedAsAFailure)
{
	// The ordinary end of a successful browse: the peer sends its last
	// directory and drops the connection, and the disconnect path asks for a
	// failure without knowing the browse just succeeded.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.OnDirectoryList(ALICE, 1, 1000);
	ASSERT_TRUE(s.OnListingReceived(ALICE, 1000).effect == Effect::Announce);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);

	ASSERT_TRUE(s.Fail(ALICE).effect == Effect::Nothing);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
}

// --- Completion, both protocol forms. ---------------------------------

TEST(BrowseStore, AFlatBrowseCompletesOnItsSingleAnswer)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_TRUE(s.OnListingReceived(ALICE, 1000).effect == Effect::Announce);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
}

TEST(BrowseStore, ADirectoryBrowseCompletesOnItsLastListing)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_TRUE(s.OnDirectoryList(ALICE, 2, 1000).effect == Effect::Nothing);
	ASSERT_TRUE(s.OnListingReceived(ALICE, 1000).effect == Effect::Nothing);
	ASSERT_EQUALS(static_cast<std::uint16_t>(50), s.BarValue(kSidA));
	ASSERT_TRUE(s.OnListingReceived(ALICE, 1000).effect == Effect::Announce);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
	ASSERT_EQUALS(static_cast<std::uint16_t>(0xffff), s.BarValue(kSidA));
}

TEST(BrowseStore, APeerWithNoDirectoriesCompletesImmediately)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_TRUE(s.OnDirectoryList(ALICE, 0, 1000).effect == Effect::Announce);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
}

// --- Silence, and what pushes it back. --------------------------------

TEST(BrowseStore, SilenceExpiresTheBrowse)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_TRUE(s.Tick(1000 + kSilenceTimeoutMs).empty());

	const auto todo = s.Tick(1001 + kSilenceTimeoutMs);
	ASSERT_EQUALS(static_cast<std::size_t>(1), todo.size());
	ASSERT_TRUE(todo[0].effect == Effect::AnnounceFailure);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
}

TEST(BrowseStore, EverySignOfLifePushesTheDeadlineBack)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 0);
	// The request going out...
	s.Touch(ALICE, 100000);
	ASSERT_TRUE(s.Tick(100000 + kSilenceTimeoutMs).empty());
	// ...the directory answer...
	s.OnDirectoryList(ALICE, 2, 200000);
	ASSERT_TRUE(s.Tick(200000 + kSilenceTimeoutMs).empty());
	// ...and each listing.
	s.OnListingReceived(ALICE, 300000);
	ASSERT_TRUE(s.Tick(300000 + kSilenceTimeoutMs).empty());
	ASSERT_TRUE(s.StateOf(kSidA) == State::InProgress);
}

TEST(BrowseStore, TouchDoesNotReviveATerminalBrowse)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Fail(ALICE);
	s.Touch(ALICE, 999999);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
}

// --- The peer going away. ---------------------------------------------

TEST(BrowseStore, ForgetFailsARunningBrowseThenReleasesThePeer)
{
	// Order matters: the failure is reported while the caller still holds the
	// reference it needs to name the peer in the log line.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	const auto effects = s.Forget(ALICE);
	ASSERT_EQUALS(static_cast<std::size_t>(2), effects.size());
	ASSERT_TRUE(effects[0].effect == Effect::AnnounceFailure);
	ASSERT_TRUE(effects[1].effect == Effect::ReleaseClient);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchIdFor(ALICE));
	// The record survives its peer.
	ASSERT_TRUE(s.Has(kSidA));
}

TEST(BrowseStore, ForgettingAFinishedBrowseAnnouncesNothing)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Finish(ALICE);
	const auto effects = s.Forget(ALICE);
	ASSERT_EQUALS(static_cast<std::size_t>(1), effects.size());
	ASSERT_TRUE(effects[0].effect == Effect::ReleaseClient);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
}

TEST(BrowseStore, ForgettingAPeerWithNoBrowseDoesNothing)
{
	Store s;
	ASSERT_TRUE(s.Forget(ALICE).empty());
}

TEST(BrowseStore, PacketsFromAForgottenPeerAreIgnored)
{
	// The reference is gone, so the peer no longer resolves to its record --
	// a late packet must not resurrect it or touch somebody else's.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Forget(ALICE);
	ASSERT_TRUE(s.OnListingReceived(ALICE, 2000).effect == Effect::Nothing);
	ASSERT_TRUE(s.OnDirectoryList(ALICE, 3, 2000).effect == Effect::Nothing);
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
}

// --- Disposal. ---------------------------------------------------------

TEST(BrowseStore, RemoveDisposesOfTheRecordWithItsSearch)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Finish(ALICE);
	s.Remove(kSidA);
	ASSERT_FALSE(s.Has(kSidA));
	ASSERT_EQUALS(static_cast<std::size_t>(0), s.Size());
	// ...and the peer is browsable again.
	ASSERT_TRUE(s.Start(ALICE, kSidB, 7, 2000).started);
}

TEST(BrowseStore, IdsEnumeratesEveryTrackedBrowse)
{
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Start(BOB, kSidB, 8, 1000);
	const auto ids = s.Ids();
	ASSERT_EQUALS(static_cast<std::size_t>(2), ids.size());
	ASSERT_EQUALS(kSidA, ids[0]);
	ASSERT_EQUALS(kSidB, ids[1]);
}

TEST(BrowseStore, UnknownSearchIdsAnswerSafely)
{
	// The EC reply paths gate on Has(), but the accessors must not invent a
	// running browse for an ID nobody is tracking.
	Store s;
	ASSERT_FALSE(s.Has(kSidA));
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
	ASSERT_EQUALS(static_cast<std::uint16_t>(0xffff), s.BarValue(kSidA));
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchIdFor(nullptr));
}

// --- Composed: the rules above are each right, and were wrong together. -

TEST(BrowseStore, ForgettingAnEndedBrowseStillNamesItForRelease)
{
	// Both halves of this were already tested apart -- that Forget releases
	// the peer, and that a finished browse no longer answers as the peer's
	// live one. Composed, the release came back with no browse attached to it,
	// so the owner had nothing to release and kept the peer allocated until
	// the user closed the tab. Most peers are in this state by the time they
	// go away: the listing arrives, then the connection closes.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.OnListingReceived(ALICE, 1000); // flat browse: finished
	ASSERT_TRUE(s.StateOf(kSidA) == State::Finished);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchIdFor(ALICE));

	const auto effects = s.Forget(ALICE);
	ASSERT_EQUALS(static_cast<std::size_t>(1), effects.size());
	ASSERT_TRUE(effects[0].effect == Effect::ReleaseClient);
	ASSERT_EQUALS(kSidA, effects[0].searchId);
}

TEST(BrowseStore, EveryOutcomeCarriesTheBrowseItHappenedTo)
{
	// Nothing may report an effect without saying which browse it belongs to;
	// an owner acting on the pair cannot then act on the wrong one, or on
	// none.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_EQUALS(kSidA, s.OnDirectoryList(ALICE, 1, 1000).searchId);
	ASSERT_EQUALS(kSidA, s.OnListingReceived(ALICE, 1000).searchId);
	const auto todo = s.Tick(2000);
	ASSERT_EQUALS(static_cast<std::size_t>(1), todo.size());
	ASSERT_EQUALS(kSidA, todo[0].searchId);

	Store s2;
	s2.Start(BOB, kSidB, 8, 1000);
	ASSERT_EQUALS(kSidB, s2.Fail(BOB).searchId);
}

TEST(BrowseStore, APeerIsBrowsableAgainAsSoonAsItsBrowseEnds)
{
	// Start refused while a terminal record still held the peer, but
	// SearchIdFor said the peer had no browse -- so the caller's own check
	// passed and the start it made was silently rejected. The two now agree:
	// only a RUNNING browse blocks a new one.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Fail(ALICE);
	// The peer has not been released yet -- that happens on the next tick.
	ASSERT_TRUE(s.Has(kSidA));
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchIdFor(ALICE));

	ASSERT_TRUE(s.Start(ALICE, kSidB, 7, 2000).started);
	ASSERT_EQUALS(kSidB, s.SearchIdFor(ALICE));
	// ...and the old record is untouched, still answering for its own state.
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
	ASSERT_TRUE(s.StateOf(kSidB) == State::InProgress);
}

TEST(BrowseStore, ARebrowseDisplacesThePeerFromItsOldRecord)
{
	// Re-browsing before the old record was released left two records naming
	// one peer, and a lookup by peer answered with whichever the map ordered
	// first -- the stale one, since ids ascend.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	s.Fail(ALICE);

	const auto result = s.Start(ALICE, kSidB, 7, 2000);
	ASSERT_TRUE(result.started);
	// The old record let go, and said so: its owner is holding a reference.
	ASSERT_TRUE(result.displaced.effect == Effect::ReleaseClient);
	ASSERT_EQUALS(kSidA, result.displaced.searchId);
	// The peer now resolves to the new browse, and only that one.
	ASSERT_EQUALS(kSidB, s.SearchIdFor(ALICE));
	ASSERT_TRUE(s.Fail(ALICE).searchId == kSidB);
	// The displaced record is untouched otherwise.
	ASSERT_TRUE(s.StateOf(kSidA) == State::Failed);
}

TEST(BrowseStore, AFirstBrowseDisplacesNothing)
{
	Store s;
	const auto result = s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_TRUE(result.started);
	ASSERT_TRUE(result.displaced.effect == Effect::Nothing);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), result.displaced.searchId);
}

TEST(BrowseStore, AnIdAlreadyInUseIsRefused)
{
	// Ids come from an allocator, but a record keyed by one already tracked
	// would silently replace it along with whatever state it held.
	Store s;
	s.Start(ALICE, kSidA, 7, 1000);
	ASSERT_FALSE(s.Start(BOB, kSidA, 8, 1000).started);
	ASSERT_EQUALS(static_cast<std::size_t>(1), s.Size());
	ASSERT_EQUALS(kSidA, s.SearchIdFor(ALICE));
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), s.SearchIdFor(BOB));
}
