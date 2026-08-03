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

#ifndef ECIDDIFF_H
#define ECIDDIFF_H

#include <algorithm>  // Needed for std::adjacent_find
#include <cassert>    // Needed for assert
#include <functional> // Needed for std::greater_equal
#include <vector>

#include "Types.h" // Needed for uint32

/**
 * Which ECIDs the client was told about last cycle and is no longer being
 * told about, i.e. the ones that need an explicit `EC_TAG_FILE_REMOVED`.
 *
 * Both inputs must be sorted ascending and duplicate-free, which lets this be
 * a single linear pass over two contiguous arrays instead of a lookup per
 * file. `Get_EC_Response_GetUpdate` gets that ordering for free: it walks the
 * encoder map, which is kept sorted by ECID.
 *
 * Deliberately pure, and deliberately not taking a CECPacket. The failure
 * mode of getting this wrong is silent in both directions -- a missed removal
 * leaves an entry in the client's list forever with no error anywhere, and a
 * spurious one deletes a file the user still has -- so it is worth being able
 * to test it without an app, a daemon, or a connected client.
 *
 * @param previous ECIDs sent in the previous response.
 * @param current  ECIDs being sent in this one.
 * @param removed  Receives the difference, ascending. Cleared first.
 */
inline void ComputeRemovedIds(
	const std::vector<uint32> &previous, const std::vector<uint32> &current, std::vector<uint32> &removed)
{
	// Preconditions, not defensive coding. An input that is out of order, or
	// that carries a duplicate, makes this quietly return the wrong set
	// rather than fail -- and both wrong answers are invisible at runtime: a
	// missed removal leaves a row in the client's list for the life of the
	// connection, and a spurious one deletes a file the user still has.
	// Checking turns that into a debug-run abort.
	//
	// One pass covers both halves of the contract: adjacent_find looks for the
	// first neighbouring pair that is not strictly increasing, which catches an
	// equal pair as well as an out-of-order one. O(n) against a merge that is
	// already O(n), and only in debug builds.
	//
	// adjacent_find rather than is_sorted with a `<=` comparator, which is what
	// this was. is_sorted requires its comparator to be a strict weak ordering
	// and `<=` is not irreflexive, so a hardened standard library is entitled
	// to -- and libstdc++ under _GLIBCXX_DEBUG does -- abort on the predicate
	// itself. That would fire on the debug build these checks exist to serve.
	// adjacent_find takes a plain binary predicate with no ordering contract.
	//
	// assert() rather than the wxASSERT used elsewhere in the tree, because
	// this header is deliberately usable without an app and wxASSERT is not:
	// wx installs its assert handler from the wxApp constructor, so with no
	// wxApp -- which is exactly the case in the unit tests -- wxASSERT is a
	// silent no-op and the check would not fire where it is easiest to
	// exercise. Verified by feeding it unsorted and duplicate input.
	assert(std::adjacent_find(previous.begin(), previous.end(), std::greater_equal<uint32>()) ==
		previous.end());
	assert(std::adjacent_find(current.begin(), current.end(), std::greater_equal<uint32>()) ==
		current.end());

	removed.clear();
	std::vector<uint32>::const_iterator cur = current.begin();
	const std::vector<uint32>::const_iterator curEnd = current.end();
	for (const uint32 prev : previous) {
		// `previous` ascends too, so the cursor only ever moves forward
		// across the whole call -- this is O(previous + current), not
		// O(previous * current).
		while (cur != curEnd && *cur < prev) {
			++cur;
		}
		if (cur == curEnd || *cur != prev) {
			removed.push_back(prev);
		}
	}
}

#endif // ECIDDIFF_H
