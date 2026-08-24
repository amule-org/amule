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

#include "MediaProbeThread.h"

#include <wx/app.h> // Needed for wxTheApp / wxQueueEvent

#include "Logger.h"
#include "MediaProbe.h"  // Needed for MediaProbe::Probe / MediaInfo
#include "ThreadTasks.h" // Needed for CMediaProbeEvent

namespace
{
// Wall-clock ceiling for a single ffprobe run. A local media file probes in
// tens of milliseconds; anything approaching this is a hung/pathological
// invocation the worker kills rather than blocking on. Also bounds how long
// a shutdown can wait on an in-flight probe (EndThread flips m_bRun, which
// Probe polls, so a stuck child is usually killed well before this).
constexpr unsigned kProbeTimeoutMs = 30000;
} // namespace

CMediaProbeThread::CMediaProbeThread()
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

void CMediaProbeThread::EndThread()
{
	{
		wxMutexLocker lock(m_mutex);
		m_bRun = false;
		m_bWorkPending = true;
		m_condition.Signal();
	}
	Wait();
}

void CMediaProbeThread::QueueProbe(
	const CMD4Hash &hash, const CPath &fullPath, const wxString &ffprobePath, bool bulk)
{
	MediaProbeJob job;
	job.hash = hash;
	job.path = fullPath;
	job.ffprobePath = ffprobePath;
	job.bulk = bulk;

	wxMutexLocker lock(m_mutex);
	m_jobList.push_back(job);
	m_bWorkPending = true;
	m_condition.Signal();
}

void *CMediaProbeThread::Entry()
{
	m_bRun = true;
	AddDebugLogLineN(logMediaProbe, wxT("Media probe thread: started"));

	// Accumulated across the drains of one bulk operation; see the summary at
	// the end of the loop for why they cannot be per-drain.
	unsigned bulkProbed = 0;
	unsigned bulkFailed = 0;

	for (;;) {
		std::list<MediaProbeJob> workList;
		{
			wxMutexLocker lock(m_mutex);
			if (m_bRun && !m_bWorkPending) {
				m_condition.WaitTimeout(500);
			}
			m_bWorkPending = false;
			// On shutdown, drop any queued probes: metadata is
			// best-effort, and unlike the hash thread there is no
			// pending-count gate that anything waits on.
			if (!m_bRun) {
				break;
			}
			workList.swap(m_jobList);
		}

		// Bulk-ness now rides on the job, set by whoever scheduled it. It
		// used to be `workList.size() > 1`, which asked the wrong question:
		// the worker swaps the whole pending list out as soon as it is
		// signalled, so the size of a batch reflects the timing of that wake
		// and nothing else. During one share scan some drains hold a single
		// job and some hold dozens, so exactly which files printed a per-file
		// line was decided by the scheduler -- one file named, the rest
		// summarised, with nothing distinguishing them (issue #1116).
		unsigned probed = 0;
		unsigned failed = 0;
		bool anyBulk = false;

		for (const MediaProbeJob &job : workList) {
			// Shut down promptly rather than draining a long backlog.
			if (!m_bRun) {
				break;
			}
			// An empty path means the user never pinned one, so fall back to
			// what this machine has. DetectedPath() memoises, so only the
			// first job in the process pays for the scan; when it finds
			// nothing every job lands here and is dropped without a word --
			// the one line explaining why was logged by that first call.
			const wxString exe =
				job.ffprobePath.IsEmpty() ? MediaProbe::DetectedPath() : job.ffprobePath;
			if (exe.IsEmpty()) {
				continue;
			}
			// Part of a bulk operation either because it was scheduled that
			// way, or because one is still draining: the tail of a share
			// import arrives a file at a time, long after the walk that found
			// it, and the scheduler can no longer tell those files from a
			// single file dropped into a shared directory. The worker can --
			// it is still finishing the same operation. Without this a large
			// first import ends with a handful of per-file lines for no
			// reason the user can see.
			const bool jobBulk = job.bulk || bulkProbed > 0;
			MediaInfo info;
			++probed;
			if (MediaProbe::Probe(exe, job.path, info, kProbeTimeoutMs, m_bRun, jobBulk)) {
				// Marshal the result to the main thread, which
				// resolves the hash to the CKnownFile and attaches
				// the FT_MEDIA_* tags (doing that here would race the
				// publish paths that read m_taglist).
				CMediaProbeEvent evt(job.hash, info);
				wxQueueEvent(wxTheApp, evt.Clone());
			} else {
				++failed;
				// Report the failure to the main thread as well, so the file
				// can be marked as tried-and-produced-nothing. Otherwise it
				// keeps no trace of having been probed and is re-queued on
				// every reload and every restart, to fail again identically.
				MediaInfo empty;
				CMediaProbeEvent evt(job.hash, empty, /*succeeded=*/false);
				wxQueueEvent(wxTheApp, evt.Clone());
			}
			anyBulk = anyBulk || jobBulk;
		}

		// One line for the whole OPERATION, not one per drain. The worker
		// takes whatever is queued each time it wakes, so a single share scan
		// is drained in several batches -- which used to produce several
		// summaries ("from 7 files", "from 33 files", "from 42 files") for
		// what the user experienced as one action, with no way to tell they
		// belonged together or that the last one was the last (issue #1116).
		// The counts accumulate across consecutive bulk drains and are
		// reported once, when nothing is left queued.
		if (anyBulk) {
			bulkProbed += probed;
			bulkFailed += failed;
		}
		bool queueEmpty;
		{
			wxMutexLocker lock(m_mutex);
			queueEmpty = m_jobList.empty();
		}
		if (queueEmpty && bulkProbed > 0) {
			if (bulkFailed > 0) {
				AddLogLineN(
					CFormat(wxPLURAL("Finished extracting media metadata from %u shared "
							 "file (%u failed)",
						"Finished extracting media metadata from %u shared "
						"files (%u failed)",
						bulkProbed)) %
					bulkProbed % bulkFailed);
			} else {
				AddLogLineN(CFormat(wxPLURAL("Finished extracting media metadata from %u "
							     "shared file",
						    "Finished extracting media metadata from %u shared files",
						    bulkProbed)) %
					    bulkProbed);
			}
			bulkProbed = 0;
			bulkFailed = 0;
		}
	}

	AddDebugLogLineN(logMediaProbe, wxT("Media probe thread: stopped"));
	return nullptr;
}
