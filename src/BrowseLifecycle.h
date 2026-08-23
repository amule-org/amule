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

#ifndef BROWSELIFECYCLE_H
#define BROWSELIFECYCLE_H

#include <cstdint>

/**
 * The state machine behind a "View Files" browse, with no dependency on
 * theApp, the client, or the clock.
 *
 * It lives apart from CBrowseManager so it can be driven directly by a test.
 * That is not incidental: every defect in this area so far has been this
 * machine going wrong in a place that only a live peer of the right kind could
 * reach -- a browse that completes without being marked finished, a peer we
 * decline to contact, a terminal browse still sitting on the pending list --
 * and none of those were reachable from a unit test while the logic was spread
 * across CUpDownClient, CSearchList and the packet handlers.
 */
namespace browse
{

//! Where a browse is in its life. Mirrors EBrowseStatus on the wire side.
enum class State
{
	InProgress, //!< asked for; nothing terminal has happened yet
	Finished,   //!< the peer sent its whole list
	Failed      //!< denied, unreachable, disconnected, or given up on
};

//! What the periodic tick should do with a record.
enum class Action
{
	None,     //!< still legitimately in flight
	Complete, //!< everything asked for has arrived: mark it finished
	Expire,   //!< nothing has arrived for too long: give up on it
	Drop      //!< already terminal: stop tracking it
};

/**
 * `outstanding` for a browse answered in a single packet (OP_ASKSHAREDFILES),
 * as opposed to the directory-by-directory form (OP_ASKSHAREDDIRS).
 *
 * The distinction is the reason a successful flat browse used to hang: the
 * directory form counts down to 0 and the packet handler marked it finished
 * there, while the flat form had no counter to reach 0 and nothing marked it
 * at all. Modelling "one answer expected" explicitly means both forms complete
 * through the same rule.
 */
constexpr int kFlatBrowse = -1;

/**
 * How long a browse may sit with nothing arriving before it is given up on.
 *
 * Refreshed on every sign of progress -- the request going out, each directory
 * landing -- so it bounds silence, not the total time a large share takes to
 * stream in.
 *
 * A backstop, not the mechanism: a browse normally ends when the peer answers,
 * denies, or the socket dies. This exists because TryToContact has several
 * exits that contact nobody, and two attempts at enumerating them both came up
 * short (amule-org/amule#1071). At 120s it sits above the 40s ed2k socket
 * timeout, so it never preempts the ordinary paths.
 */
constexpr std::uint64_t kSilenceTimeoutMs = 120000;

//! One browse, start to finish.
struct Record
{
	//! Search ID this browse's results are filed under. Allocated when the
	//! browse is asked for, so it is never 0 for a live record.
	std::uint32_t searchId = 0;
	//! ECID of the peer being browsed, for the listing and the GUI tab.
	std::uint32_t peerEcid = 0;
	State state = State::InProgress;
	//! Directory listings still expected, or kFlatBrowse.
	int outstanding = kFlatBrowse;
	//! Directories the peer said it has; 0 until it answers, and always 0 for
	//! a flat browse. Only used for the progress percent.
	int totalDirs = 0;
	//! Tick count after which silence means the browse is dead. Never 0 for a
	//! record still in progress.
	std::uint64_t deadline = 0;
};

/**
 * What to do with `rec` at time `now`.
 *
 * Completion is checked before expiry: a browse whose last directory arrived
 * in the same tick that its deadline passed has succeeded, not timed out.
 */
Action Tick(const Record &rec, std::uint64_t now);

/**
 * Progress bar value: 0..100 while running, or 0xffff once terminal, which is
 * the sentinel the search list's bar already uses to mean "clear it".
 *
 * A browse with no directory count -- still connecting, or flat -- has no
 * meaningful percent and reports 0 rather than guessing.
 */
std::uint16_t BarValue(const Record &rec);

/**
 * Apply `action` to `rec`, returning the record it leaves behind.
 *
 * A browse that has already ended stays ended. That is the whole rule, and it
 * has to live here rather than at the call sites: the peer disconnecting
 * immediately after sending its last directory is the ordinary case, not an
 * edge one, and the disconnect path asks for a failure without knowing the
 * browse just succeeded. Deciding it at each caller is how the invariant got
 * broken five times before it had an owner.
 *
 * Action::Drop is not a state change and is handled by the caller, which knows
 * what tracking means.
 */
Record ApplyAction(Record rec, Action action);

/**
 * The peer answered OP_ASKSHAREDDIRS with `dirCount` directories.
 *
 * `dirCount == 0` is a real answer, not a malformed one: the peer is telling
 * us it has nothing to send, which completes the browse rather than leaving it
 * waiting for listings that will never come.
 */
Record OnDirectoryList(Record rec, int dirCount, std::uint64_t deadline);

/**
 * One directory's worth of files arrived (either form), so the browse is alive
 * and one step further along.
 */
Record OnListingReceived(Record rec, std::uint64_t deadline);

} // namespace browse

#endif // BROWSELIFECYCLE_H
