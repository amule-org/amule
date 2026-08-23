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

#include <BrowseLifecycle.h>

using namespace muleunit;
using namespace browse;

DECLARE_SIMPLE(BrowseLifecycle)

namespace
{
//! A browse just asked for, over the directory-by-directory protocol form.
Record DirBrowse(std::uint64_t deadline = 1000)
{
	Record r;
	r.searchId = 42;
	r.state = State::InProgress;
	r.outstanding = kFlatBrowse; // until the peer answers with a count
	r.deadline = deadline;
	return r;
}
} // namespace

// --- The five defects this machine exists to make unreachable. ---------
// Each of these was a live bug that only the right kind of peer could reach:
// amule-org/amule#1071 and its two follow-ups. They are the reason the
// lifecycle was pulled out of CUpDownClient at all.

TEST(BrowseLifecycle, FlatBrowseCompletesWhenItsSingleAnswerArrives)
{
	// A peer without directory support (MLdonkey, plain eDonkey, eMule < 0.28)
	// answers OP_ASKSHAREDFILES in one packet. That used to leave the browse
	// BROWSE_IN_PROGRESS forever -- a *successful* browse that never ended,
	// with no timeout able to touch it.
	Record r = DirBrowse();
	ASSERT_TRUE(Tick(r, 1) == Action::None);

	r = OnListingReceived(r, 2000);
	ASSERT_EQUALS(0, r.outstanding);
	ASSERT_TRUE(Tick(r, 1) == Action::Complete);
}

TEST(BrowseLifecycle, ZeroDirectoriesCompletesRatherThanWaiting)
{
	// A peer answering "I have 0 directories" is a real answer. It used to set
	// the counter to 0 with nothing marking the browse finished, leaving it
	// hanging at 0%.
	Record r = DirBrowse();
	r = OnDirectoryList(r, 0, 2000);
	ASSERT_EQUALS(0, r.outstanding);
	ASSERT_TRUE(Tick(r, 1) == Action::Complete);
}

TEST(BrowseLifecycle, TerminalRecordIsDroppedNotRetriedForever)
{
	// The pending list used to key its erase on the deadline rather than the
	// state, so a browse that ended without clearing its deadline was
	// re-examined on every core tick for the life of the client.
	for (State s : { State::Finished, State::Failed }) {
		Record r = DirBrowse();
		r.state = s;
		ASSERT_TRUE(Tick(r, 1) == Action::Drop);
		ASSERT_TRUE(Tick(r, 999999) == Action::Drop);
	}
}

TEST(BrowseLifecycle, SilenceExpiresOnlyAfterTheDeadline)
{
	Record r = DirBrowse(1000);
	r = OnDirectoryList(r, 3, 1000);
	ASSERT_TRUE(Tick(r, 999) == Action::None);
	ASSERT_TRUE(Tick(r, 1000) == Action::None); // not yet: strictly after
	ASSERT_TRUE(Tick(r, 1001) == Action::Expire);
}

TEST(BrowseLifecycle, ProgressPushesTheDeadlineBackSoALiveStreamSurvives)
{
	// The deadline bounds silence, not duration: a large share may stream for
	// far longer than the timeout as long as something keeps arriving.
	Record r = DirBrowse(1000);
	r = OnDirectoryList(r, 3, 1000);
	// Each directory lands just before the previous deadline would have run
	// out, so total elapsed time (2700) far exceeds the 1000-tick timeout.
	std::uint64_t t = 900;
	for (int dir = 0; dir < 3; ++dir, t += 900) {
		ASSERT_TRUE(Tick(r, t) == Action::None);
		r = OnListingReceived(r, t + 1000);
	}
	// Three listings for three directories: complete, never expired.
	ASSERT_EQUALS(0, r.outstanding);
	ASSERT_TRUE(Tick(r, t) == Action::Complete);
}

TEST(BrowseLifecycle, CompletionWinsOverAnExpiryInTheSameTick)
{
	// The last listing landing exactly as the deadline passes is a success.
	// Reporting it as a timeout would discard results already in hand.
	Record r = DirBrowse();
	r = OnDirectoryList(r, 1, 500);
	r = OnListingReceived(r, 500);
	ASSERT_EQUALS(0, r.outstanding);
	ASSERT_TRUE(Tick(r, 100000) == Action::Complete);
}

// --- Progress reporting. ----------------------------------------------

TEST(BrowseLifecycle, BarTracksDirectoriesReceived)
{
	Record r = DirBrowse();
	ASSERT_EQUALS(static_cast<std::uint16_t>(0), BarValue(r)); // no count yet
	r = OnDirectoryList(r, 4, 2000);
	ASSERT_EQUALS(static_cast<std::uint16_t>(0), BarValue(r));
	r = OnListingReceived(r, 2000);
	ASSERT_EQUALS(static_cast<std::uint16_t>(25), BarValue(r));
	r = OnListingReceived(r, 2000);
	ASSERT_EQUALS(static_cast<std::uint16_t>(50), BarValue(r));
}

TEST(BrowseLifecycle, BarClearsOnceTerminal)
{
	// 0xffff is the sentinel the search list's bar already reads as "clear".
	Record r = DirBrowse();
	r = OnDirectoryList(r, 2, 2000);
	r.state = State::Finished;
	ASSERT_EQUALS(static_cast<std::uint16_t>(0xffff), BarValue(r));
	r.state = State::Failed;
	ASSERT_EQUALS(static_cast<std::uint16_t>(0xffff), BarValue(r));
}

TEST(BrowseLifecycle, FlatBrowseReportsNoPercent)
{
	// Nothing to count against, so it reports 0 rather than inventing progress.
	Record r = DirBrowse();
	ASSERT_EQUALS(static_cast<std::uint16_t>(0), BarValue(r));
	ASSERT_EQUALS(kFlatBrowse, r.outstanding);
}

// --- Late and duplicate packets. --------------------------------------

TEST(BrowseLifecycle, PacketsForATerminalBrowseChangeNothing)
{
	// A peer that answers after we gave up must not resurrect the record --
	// the search id may already have been reused for a fresh browse of the
	// same peer.
	Record failed = DirBrowse();
	failed.state = State::Failed;

	Record after = OnDirectoryList(failed, 5, 9999);
	ASSERT_TRUE(after.state == State::Failed);
	ASSERT_EQUALS(kFlatBrowse, after.outstanding);
	ASSERT_EQUALS(static_cast<std::uint64_t>(1000), after.deadline);

	after = OnListingReceived(failed, 9999);
	ASSERT_TRUE(after.state == State::Failed);
	ASSERT_EQUALS(kFlatBrowse, after.outstanding);
}

TEST(BrowseLifecycle, MoreListingsThanAnnouncedDoNotUnderflow)
{
	// A peer sending more directories than it announced must not drive the
	// counter negative, which would make the browse un-completable.
	Record r = DirBrowse();
	r = OnDirectoryList(r, 1, 2000);
	r = OnListingReceived(r, 2000);
	ASSERT_EQUALS(0, r.outstanding);
	r = OnListingReceived(r, 2000);
	ASSERT_EQUALS(0, r.outstanding);
	ASSERT_TRUE(Tick(r, 1) == Action::Complete);
	ASSERT_EQUALS(static_cast<std::uint16_t>(100), BarValue(r));
}

TEST(BrowseLifecycle, ADirectoryAnswerAfterListingsHaveStartedIsStillHonoured)
{
	// OP_ASKSHAREDDIRSANS is the clearest sign of life in the exchange, so it
	// must push the deadline back like any other progress.
	Record r = DirBrowse(1000);
	r = OnDirectoryList(r, 2, 5000);
	ASSERT_EQUALS(static_cast<std::uint64_t>(5000), r.deadline);
	ASSERT_TRUE(Tick(r, 1001) == Action::None);
}

// --- Terminal is terminal. ---------------------------------------------

TEST(BrowseLifecycle, ADisconnectAfterCompletionDoesNotTurnSuccessIntoFailure)
{
	// The ordinary shape of a successful browse: the peer sends its last
	// directory and then drops the connection. The disconnect path asks for a
	// failure without knowing the browse just succeeded, so the rule has to be
	// here rather than at that call site.
	Record r = DirBrowse();
	r = OnDirectoryList(r, 1, 2000);
	r = OnListingReceived(r, 2000);
	r = ApplyAction(r, Action::Complete);
	ASSERT_TRUE(r.state == State::Finished);

	r = ApplyAction(r, Action::Expire);
	ASSERT_TRUE(r.state == State::Finished);
}

TEST(BrowseLifecycle, AFailedBrowseIsNotCompletedByALatePacket)
{
	Record r = DirBrowse();
	r = ApplyAction(r, Action::Expire);
	ASSERT_TRUE(r.state == State::Failed);

	r = ApplyAction(r, Action::Complete);
	ASSERT_TRUE(r.state == State::Failed);
}

TEST(BrowseLifecycle, DropAndNoneAreNotStateChanges)
{
	Record r = DirBrowse();
	ASSERT_TRUE(ApplyAction(r, Action::None).state == State::InProgress);
	ASSERT_TRUE(ApplyAction(r, Action::Drop).state == State::InProgress);
}
