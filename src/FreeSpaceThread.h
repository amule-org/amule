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

#ifndef FREESPACETHREAD_H
#define FREESPACETHREAD_H

#include <atomic>
#include <wx/thread.h>

#include "Types.h"       // Needed for uint64 / sint64
#include <common/Path.h> // Needed for CPath (value members)

/**
 * Dedicated worker sampling the free space on the temp and incoming
 * directories, published through CStatistics for the Downloads and Shared
 * Files panels.
 *
 * Why its own thread: statvfs() / GetDiskFreeSpaceEx() blocks on the
 * directory it is asked about. On a local disk that is microseconds, but
 * either directory is commonly a network mount, and there the healthy
 * sub-millisecond case is not what matters -- a cold autofs mount takes
 * tens to hundreds of milliseconds, and an unreachable server blocks for
 * the mount's timeout, unbounded on a hard mount. The two callers are the
 * GUI timer on the main thread and the EC stats reply on the core thread,
 * so an inline probe puts that stall in front of the whole application.
 *
 * Nothing else in the core reaches those filesystems from the main loop:
 * downloads write through CPartFileWriteThread and uploads read through
 * CUploadDiskIOThread. This probe would have been the one exception.
 *
 * Isolated rather than queued onto CThreadScheduler for the same reason
 * CMediaProbeThread is: that scheduler runs one task at a time and owns
 * completion, allocation and hashing, so a probe blocked on a hung mount
 * would not delay that queue but stop it -- and would couple unrelated
 * filesystems, a hung incoming mount blocking the hashing of part files on
 * a healthy local disk. Here a hung mount can only ever delay the next
 * sample: the figure goes stale, the label empties, and nothing else
 * notices.
 */
class CFreeSpaceThread : public wxThread
{
public:
	CFreeSpaceThread();
	// EndThread() (which joins the worker) must be called before the
	// object is destroyed; the destructor itself has nothing to do.
	~CFreeSpaceThread() = default;

	//! Signal the worker to stop and join it.
	void EndThread();

	/**
	 * Hands the worker the directories to sample.
	 *
	 * Called from the main thread, which is the only one that may read
	 * thePrefs -- the paths can change while running (the preferences
	 * dialog), and no worker in the tree touches preferences directly.
	 * Cheap enough to call on every core tick, which is what keeps the
	 * snapshot current.
	 */
	void SetPaths(const CPath &tempDir, const CPath &incomingDir);

	bool IsRunning() const { return m_bRun; }

private:
	void *Entry() override;

	//! One blocking probe. FREE_SPACE_UNKNOWN if the path can't be queried.
	static sint64 Sample(const CPath &path);

	std::atomic<bool> m_bRun{ false };

	wxMutex m_mutex;
	wxCondition m_condition;

	// Snapshots taken from thePrefs by the main thread; read by the
	// worker. Both protected by m_mutex.
	CPath m_tempDir;
	CPath m_incomingDir;
};

#endif // FREESPACETHREAD_H
