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
			MediaInfo info;
			++probed;
			if (MediaProbe::Probe(exe, job.path, info, kProbeTimeoutMs, m_bRun, job.bulk)) {
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
			anyBulk = anyBulk || job.bulk;
		}

		// One line for the whole drain. Only in bulk mode -- a single-file
		// batch already said its piece per file, and saying it twice would be
		// noise. Failures are named in the summary rather than hidden: the
		// point of reporting them at all is that a misconfigured ffprobe must
		// not look like success.
		if (anyBulk && probed > 0) {
			if (failed > 0) {
				AddLogLineN(
					CFormat(wxPLURAL("Extracted media metadata from %u file (%u failed)",
						"Extracted media metadata from %u files (%u failed)",
						probed)) %
					probed % failed);
			} else {
				AddLogLineN(CFormat(wxPLURAL("Extracted media metadata from %u file",
						    "Extracted media metadata from %u files",
						    probed)) %
					    probed);
			}
		}
	}

	AddDebugLogLineN(logMediaProbe, wxT("Media probe thread: stopped"));
	return nullptr;
}
