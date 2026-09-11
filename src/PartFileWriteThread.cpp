//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "PartFileWriteThread.h"

#include "PartFile.h" // Needed for CPartFile, PartFileBufferedData
#include "CFile.h"    // Needed for CIOFailureException
#include "Logger.h"
#include <common/Format.h> // Needed for CFormat

// eMule ref: CPartFileWriteThread::CPartFileWriteThread()
CPartFileWriteThread::CPartFileWriteThread()
: wxThread(wxTHREAD_JOINABLE)
, m_condition(m_mutex)
{
	m_bRun = false;
	m_bWorkPending = false;

	wxMutexLocker lock(m_mutex);
	if (Create() == wxTHREAD_NO_ERROR) {
		Run();
	}
}

CPartFileWriteThread::~CPartFileWriteThread()
{
	// EndThread() must have been called before destruction.
}

// eMule ref: CPartFileWriteThread::EndThread()
void CPartFileWriteThread::EndThread()
{
	{
		wxMutexLocker lock(m_mutex);
		m_bRun = false;
		m_bWorkPending = true;
		m_condition.Signal();
	}
	Wait();
}

// Called by the main thread to queue a write item.
// eMule ref: CPartFileWriteThread::WakeUpCall()
void CPartFileWriteThread::QueueWrite(CPartFile *pFile, PartFileBufferedData *pBuffer)
{
	wxMutexLocker lock(m_mutex);
	m_flushList.push_back(ToWrite{ pFile, pBuffer });
	m_bWorkPending = true;
	m_condition.Signal();
}

void CPartFileWriteThread::DropReferencesTo(const CKnownFile *file)
{
	// Pointer-value strip of any pending write item whose pFile matches `file`. Called from
	// MuleNotify::KnownFileBeingDestroyed before CPartFile is freed, or the write loop would
	// deref the dangling pFile on the next tick. CPartFile inherits from CKnownFile at the same
	// address, so the cast never derefs.
	//
	// pBuffer is NOT deleted here: ownership stays with CPartFile::m_BufferedData_list, which
	// ~CPartFile frees, so deleting it here too would double-free.
	wxMutexLocker lock(m_mutex);
	for (std::list<ToWrite>::iterator it = m_flushList.begin(); it != m_flushList.end();
		/* manual ++ */) {
		if (static_cast<const CKnownFile *>(it->pFile) == file) {
			it = m_flushList.erase(it);
		} else {
			++it;
		}
	}
}

// Replaces eMule's IOCP + overlapped WriteFile with synchronous CFileArea::FlushAt(). The thread is
// dedicated to writes, so blocking on disk I/O is fine -- the win is that the main thread no longer
// stalls.
void *CPartFileWriteThread::Entry()
{
	m_bRun = true;

	// Loop until EndThread() clears m_bRun, then drain one final batch before returning. A
	// shutdown signal must never drop queued writes: WriteToBuffer FillGap()s each range at
	// queue time and the gaplist is persisted, so a dropped write would leave the .met claiming
	// bytes that never reached disk -- a silently corrupt part on the next launch.
	for (;;) {
		// Move queued items to a local work list under the lock, so the main thread
		// can keep queueing while we process it.
		std::list<ToWrite> workList;
		bool keepRunning;
		{
			wxMutexLocker lock(m_mutex);
			if (m_bRun && !m_bWorkPending) {
				m_condition.WaitTimeout(500);
			}
			m_bWorkPending = false;
			workList.swap(m_flushList);
			// Captured under the lock together with the swap, so the
			// batch we just took is drained whatever m_bRun does next.
			keepRunning = m_bRun;
		}

		// Process all queued writes synchronously. No m_bRun check in the loop condition: a
		// batch, once taken, is always written in full so a shutdown never abandons it
		// half-drained.
		// eMule ref: WriteBuffers() -- line 122
		for (std::list<ToWrite>::iterator it = workList.begin(); it != workList.end(); ++it) {
			PartFileBufferedData *pBuffer = it->pBuffer;
			uint32 lenData = (uint32)(pBuffer->end - pBuffer->start + 1);

			// Synchronous write via CFileArea::FlushAt(), which writes the buffered
			// data at the given offset.
			//
			// Lock m_hpartfileMutex against CPartFileHashThread: with ENABLE_MMAP=OFF
			// the underlying CFileAutoClose::WriteAt does Seek+Write on the shared fd
			// while HashSinglePart does Seek+Read on the same one, and the two race on
			// the file position without it.
			//
			// FlushAt can throw CIOFailureException on a disk-full, EIO or permission
			// failure. Catching it keeps the worker alive: an unhandled exception in
			// Entry() reaches wxApp::OnUnhandledException(), whose terminate handler
			// aborts the process. On failure the item is marked PB_ERROR so the main
			// thread retries on the next FlushBuffer.
			bool writeOk = true;
			try {
				std::lock_guard<std::mutex> lock(it->pFile->m_hpartfileMutex);
				pBuffer->area.FlushAt(it->pFile->m_hpartfile, pBuffer->start, lenData);
			} catch (const CIOFailureException &e) {
				AddDebugLogLineC(logPartFile,
					CFormat("Write thread: I/O failure on '%s' at offset %llu (%u "
						"bytes): %s") %
						it->pFile->GetFileName() % pBuffer->start % lenData %
						e.what());
				writeOk = false;
			}

			// eMule ref: WriteCompletionRoutine line 179 -- decrement in write thread
			// so main thread can check m_iWrites at any time.
			--it->pFile->m_iWrites;

			// Mark the buffer written or errored so the main thread can harvest it.
			// PB_ERROR is handled in FlushBuffer Phase 2, which resets it to PB_READY
			// for retry -- and if the disk is genuinely full, CheckFreeDiskSpace pauses
			// the file before the retry loops.
			pBuffer->flushed = writeOk ? PB_WRITTEN : PB_ERROR;
		}

		// Exit only after the batch above is on disk, so EndThread()
		// genuinely drains rather than dropping in-flight writes.
		if (!keepRunning) {
			break;
		}
	}

	return NULL;
}
// File_checked_for_headers
