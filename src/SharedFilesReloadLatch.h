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

#ifndef SHAREDFILESRELOADLATCH_H
#define SHAREDFILESRELOADLATCH_H

/**
 * The "a shared-files reload is owed" flag, on its own so the rules can be
 * tested without standing up a CSharedFileList (which needs theApp, the
 * preferences and a filesystem to walk).
 *
 * Three rules, and the middle one is why this is not a plain bool:
 *
 *  - Requests coalesce. Ten callers asking before the next tick get one walk.
 *  - A request that arrives *during* a walk belongs to the NEXT walk. The walk
 *    in flight is already past the files that request was about, so it cannot
 *    satisfy it. BeginWalk() takes the outstanding request with it, which
 *    leaves anything requested afterwards standing.
 *  - A walk that aborts satisfies nothing, so EndWalk() hands the request back
 *    rather than letting a cancelled scan swallow it.
 *
 * Single-threaded by contract: every caller runs on the core event loop. That
 * is deliberate and a caller off that thread is the thing to fix, not this.
 */
class CSharedFilesReloadLatch
{
public:
	/// Ask for a walk. Repeat calls before the next BeginWalk() coalesce.
	void Request() { m_pending = true; }

	/// True while a walk is owed.
	bool IsPending() const { return m_pending; }

	/// Whether the core tick should start a walk now. A walk already running
	/// leaves the request standing for a later tick rather than nesting.
	bool ShouldStartFromTick(bool walkRunning) const { return m_pending && !walkRunning; }

	/**
	 * Called as a walk starts. Takes the outstanding request with it and
	 * returns whether there was one, so anything requested from here on is
	 * owed to the next walk. Pass the result to EndWalk().
	 */
	bool BeginWalk()
	{
		const bool had = m_pending;
		m_pending = false;
		return had;
	}

	/**
	 * Called as a walk finishes. An aborted walk gives its request back; a
	 * completed one has satisfied it. Requests that arrived mid-walk are
	 * untouched either way — they were never taken.
	 */
	void EndWalk(bool hadRequest, bool aborted)
	{
		if (aborted && hadRequest) {
			m_pending = true;
		}
	}

private:
	bool m_pending = false;
};

#endif // SHAREDFILESRELOADLATCH_H
// File_checked_for_headers
