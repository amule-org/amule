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

void CMediaProbeThread::QueueProbe(const CMD4Hash &hash, const CPath &fullPath, const wxString &ffprobePath)
{
	MediaProbeJob job;
	job.hash = hash;
	job.path = fullPath;
	job.ffprobePath = ffprobePath;

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

		for (const MediaProbeJob &job : workList) {
			// Shut down promptly rather than draining a long backlog.
			if (!m_bRun) {
				break;
			}
			MediaInfo info;
			if (MediaProbe::Probe(job.ffprobePath, job.path, info, kProbeTimeoutMs, m_bRun)) {
				// Marshal the result to the main thread, which
				// resolves the hash to the CKnownFile and attaches
				// the FT_MEDIA_* tags (doing that here would race the
				// publish paths that read m_taglist).
				CMediaProbeEvent evt(
					job.hash, info.length_seconds, info.bitrate_kbps, info.codec);
				wxQueueEvent(wxTheApp, evt.Clone());
			}
		}
	}

	AddDebugLogLineN(logMediaProbe, wxT("Media probe thread: stopped"));
	return nullptr;
}
