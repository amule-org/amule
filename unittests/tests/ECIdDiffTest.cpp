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

#include "ECIdDiff.h"

using namespace muleunit;

// `ComputeRemovedIds` decides which files the daemon tells a partial-update
// client to delete. Both ways of getting it wrong are silent: a missed removal
// leaves an entry in the client's list for the life of the connection with no
// error logged anywhere, and a spurious one deletes a file the user still has.
// Neither shows up as a crash, a failed request, or a wrong-looking packet --
// only as a list that quietly disagrees with the daemon.
//
// It is a pure function over two sorted vectors, so unlike the surrounding
// handler it needs no app, no daemon and no connected client to exercise.

DECLARE_SIMPLE(ECIdDiff)

// Convenience: run the diff and hand back the result by value.
static std::vector<uint32> Diff(const std::vector<uint32> &previous, const std::vector<uint32> &current)
{
	std::vector<uint32> removed;
	ComputeRemovedIds(previous, current, removed);
	return removed;
}

TEST(ECIdDiff, NothingChangedRemovesNothing)
{
	const std::vector<uint32> ids = { 1, 5, 9, 40 };
	ASSERT_TRUE(Diff(ids, ids).empty());
}

TEST(ECIdDiff, BothEmpty)
{
	ASSERT_TRUE(Diff(std::vector<uint32>(), std::vector<uint32>()).empty());
}

// The first poll of a connection: nothing was sent before, so however many
// files exist now, none of them can have been removed.
TEST(ECIdDiff, FirstPollRemovesNothing)
{
	ASSERT_TRUE(Diff(std::vector<uint32>(), { 3, 7, 11 }).empty());
}

TEST(ECIdDiff, EverythingGone)
{
	const std::vector<uint32> removed = Diff({ 3, 7, 11 }, std::vector<uint32>());
	ASSERT_EQUALS(3u, removed.size());
	ASSERT_EQUALS(3u, removed[0]);
	ASSERT_EQUALS(7u, removed[1]);
	ASSERT_EQUALS(11u, removed[2]);
}

// The cursor starts at the front, so a removal at the very front is the case
// an off-by-one in the initial position would miss.
TEST(ECIdDiff, RemovedAtFront)
{
	const std::vector<uint32> removed = Diff({ 1, 5, 9 }, { 5, 9 });
	ASSERT_EQUALS(1u, removed.size());
	ASSERT_EQUALS(1u, removed[0]);
}

TEST(ECIdDiff, RemovedInMiddle)
{
	const std::vector<uint32> removed = Diff({ 1, 5, 9 }, { 1, 9 });
	ASSERT_EQUALS(1u, removed.size());
	ASSERT_EQUALS(5u, removed[0]);
}

// The cursor has run off the end by the time the last id is considered, so
// this is the case a missing `cur == curEnd` guard would get wrong.
TEST(ECIdDiff, RemovedAtEnd)
{
	const std::vector<uint32> removed = Diff({ 1, 5, 9 }, { 1, 5 });
	ASSERT_EQUALS(1u, removed.size());
	ASSERT_EQUALS(9u, removed[0]);
}

// Additions must never be reported as removals -- that would delete a file
// from the client's list on the very poll it first appeared.
TEST(ECIdDiff, AdditionsAreNotRemovals)
{
	ASSERT_TRUE(Diff({ 5 }, { 1, 5, 9 }).empty());
}

// Adds and removes in the same cycle, interleaved so the cursor has to step
// over new ids while still matching the surviving ones.
TEST(ECIdDiff, AddAndRemoveInTheSameCycle)
{
	const std::vector<uint32> removed = Diff({ 10, 20, 30, 40 }, { 5, 20, 25, 40, 50 });
	ASSERT_EQUALS(2u, removed.size());
	ASSERT_EQUALS(10u, removed[0]);
	ASSERT_EQUALS(30u, removed[1]);
}

// Disjoint sets: every previous id is gone and every current one is new. The
// cursor must not be left behind by the leading run of smaller new ids.
TEST(ECIdDiff, DisjointSets)
{
	const std::vector<uint32> removed = Diff({ 100, 200 }, { 1, 2, 3 });
	ASSERT_EQUALS(2u, removed.size());
	ASSERT_EQUALS(100u, removed[0]);
	ASSERT_EQUALS(200u, removed[1]);
}

// The output is cleared, not appended to -- the handler reuses one vector.
TEST(ECIdDiff, OutputIsClearedNotAppended)
{
	std::vector<uint32> removed = { 777, 888 };
	ComputeRemovedIds({ 1, 2 }, { 1 }, removed);
	ASSERT_EQUALS(1u, removed.size());
	ASSERT_EQUALS(2u, removed[0]);
}

// ECIDs are assigned from a counter that keeps climbing across a long daemon
// uptime, so the comparisons must stay correct above the signed-32-bit range.
TEST(ECIdDiff, HighIdsCompareUnsigned)
{
	const std::vector<uint32> removed =
		Diff({ 2147483647u, 2147483648u, 4294967295u }, { 2147483647u, 4294967295u });
	ASSERT_EQUALS(1u, removed.size());
	ASSERT_EQUALS(2147483648u, removed[0]);
}
