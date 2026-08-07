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

#include "FreeSpaceThread.h" // Interface declarations

#include <wx/filefn.h> // Needed for wxInvalidOffset

#include "GetTickCount.h" // Needed for GetTickCount64()
#include "Logger.h"       // Needed for AddDebugLogLineN
#include "Statistics.h"   // Needed for FREE_SPACE_UNKNOWN and the publishers

namespace
{
/**
 * How stale a published figure may get.
 *
 * A display interval, not a cost budget: the panels report a disk filling
 * up over minutes, so ten seconds is finer than anyone can act on, and the
 * probe no longer runs anywhere that would notice a slower one.
 */
const uint64 kSampleIntervalMs = 10000;

//! How often the worker wakes to check whether a sample is due. Shorter
//! than the interval so shutdown doesn't wait out a sleeping thread.
const int kTickMs = 1000;
} // namespace

CFreeSpaceThread::CFreeSpaceThread()
: wxThread(wxTHREAD_JOINABLE)
, m_condition(m_mutex)
{
	m_bRun = false;
	wxMutexLocker lock(m_mutex);
	if (Create() == wxTHREAD_NO_ERROR) {
		Run();
	}
}

void CFreeSpaceThread::EndThread()
{
	{
		wxMutexLocker lock(m_mutex);
		m_bRun = false;
		m_condition.Signal();
	}
	// Returns as soon as the worker finishes the probe it is in, if any.
	// A probe blocked on an unreachable mount holds the join for as long
	// as that mount takes to fail -- the alternative, detaching, would
	// leave a thread writing into statics the app is tearing down.
	Wait();
}

void CFreeSpaceThread::SetPaths(const CPath &tempDir, const CPath &incomingDir)
{
	wxMutexLocker lock(m_mutex);
	m_tempDir = tempDir;
	m_incomingDir = incomingDir;
}

sint64 CFreeSpaceThread::Sample(const CPath &path)
{
	if (!path.IsOk()) {
		return FREE_SPACE_UNKNOWN;
	}
	// wxInvalidOffset means the path could not be queried at all: it does
	// not exist, or its mount is unreachable. Kept distinct from a real
	// zero, which is a full disk and exactly what the Downloads panel
	// warns about.
	const sint64 free = CPath::GetFreeSpaceAt(path);
	return (free == wxInvalidOffset) ? FREE_SPACE_UNKNOWN : free;
}

void *CFreeSpaceThread::Entry()
{
	m_bRun = true;
	AddDebugLogLineN(logGeneral, wxT("Free space thread: started"));

	// Zero means "never sampled", so both paths are probed on the first
	// pass and the panels have a figure to show straight away.
	uint64 lastTemp = 0;
	uint64 lastIncoming = 0;

	while (m_bRun) {
		CPath tempDir;
		CPath incomingDir;
		{
			wxMutexLocker lock(m_mutex);
			if (m_bRun) {
				m_condition.WaitTimeout(kTickMs);
			}
			if (!m_bRun) {
				break;
			}
			tempDir = m_tempDir;
			incomingDir = m_incomingDir;
		}

		// Timed separately so a slow incoming directory cannot hold temp
		// back, and probed outside the lock so SetPaths() never waits on
		// a filesystem.
		const uint64 now = GetTickCount64();
		if (lastTemp == 0 || now - lastTemp >= kSampleIntervalMs) {
			lastTemp = now;
			CStatistics::PublishTempFreeSpace(Sample(tempDir));
		}
		if (m_bRun && (lastIncoming == 0 || GetTickCount64() - lastIncoming >= kSampleIntervalMs)) {
			lastIncoming = GetTickCount64();
			CStatistics::PublishIncomingFreeSpace(Sample(incomingDir));
		}
	}

	AddDebugLogLineN(logGeneral, wxT("Free space thread: stopped"));
	return nullptr;
}
