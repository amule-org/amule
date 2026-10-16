//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

#ifndef FREESPACESAMPLES_H
#define FREESPACESAMPLES_H

#include "Types.h"       // Needed for sint64, uint64
#include <common/Path.h> // Needed for CPath

#include <map>
#include <mutex>
#include <vector>

/**
 * The rule a download's writes follow: never below one part free, and with the user's limit on,
 * at least that much left after writing @a needed bytes. A negative @a free (unknown) fails.
 */
bool FreeSpaceAllows(sint64 free, uint64 needed, bool checkEnabled, uint64 minFree);

/**
 * The latest free-space sample of each directory, published by CFreeSpaceThread and read by the
 * main thread, which must not probe a mount itself: on a busy or hung one the probe blocks.
 */
class CFreeSpaceSamples
{
public:
	//! @a tick: when the probe started, so a slow probe does not look newer than it is.
	void Publish(const CPath &dir, sint64 free, uint64 tick);

	//! The sample of @a dir, or -1 if there is none, it failed, it is older than @a maxAgeMs at
	//! @a now (the probe is stuck on the mount), or it was taken before @a notBefore.
	sint64 Get(const CPath &dir, uint64 now, uint64 maxAgeMs, uint64 notBefore = 0) const;

	//! Forgets the directories not in @a dirs.
	void Retain(const std::vector<CPath> &dirs);

private:
	struct Sample
	{
		sint64 free;
		uint64 tick;
	};

	mutable std::mutex m_mutex;
	std::map<wxString, Sample> m_samples;
};

#endif // FREESPACESAMPLES_H
// File_checked_for_headers
