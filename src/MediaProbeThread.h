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

#ifndef MEDIAPROBETHREAD_H
#define MEDIAPROBETHREAD_H

#include <list>
#include <atomic>
#include <wx/thread.h>
#include <wx/string.h>

#include "MD4Hash.h"     // Needed for CMD4Hash
#include <common/Path.h> // Needed for CPath (value member of MediaProbeJob)

// One queued ffprobe job. Everything is snapshotted by value at enqueue
// time so the worker never touches shared CKnownFile / thePrefs state.
struct MediaProbeJob
{
	CMD4Hash hash;        // resolved back to the CKnownFile on the main thread
	CPath path;           // file to probe
	wxString ffprobePath; // ffprobe binary
};

// Dedicated worker for ffprobe media-metadata extraction (#280).
//
// Why its own thread: media probing shells out to ffprobe via wxExecute,
// which can block for a long time (or, on a headless daemon, hang) and
// must never stall the core. Previously CMediaProbeTask ran on the shared
// CThreadScheduler alongside CHashingTask / CCompletionTask, so a single
// hung ffprobe wedged every download completion. Running probes on a
// dedicated, isolated worker means a slow/hung probe can only ever delay
// other probes, never completions or hashing. A wall-clock timeout in
// MediaProbe::Probe bounds each probe so even this thread can't wedge.
//
// Mirrors CPartFileHashThread: a single joinable worker draining a job
// queue and posting a CMediaProbeEvent back to the main thread.
class CMediaProbeThread : public wxThread
{
public:
	CMediaProbeThread();
	// EndThread() (which joins the worker) must be called before the
	// object is destroyed; the destructor itself has nothing to do.
	~CMediaProbeThread() = default;

	// Signal the worker to stop and join it. Queued (not-yet-started)
	// probes are dropped — metadata is best-effort. An in-flight probe's
	// ffprobe child is killed promptly (Probe polls the run flag), so the
	// join returns without waiting on a slow or hung ffprobe.
	void EndThread();

	// Enqueue a probe. Thread-safe; returns immediately.
	void QueueProbe(const CMD4Hash &hash, const CPath &fullPath, const wxString &ffprobePath);

	bool IsRunning() const { return m_bRun; }

private:
	void *Entry() override;

	std::atomic<bool> m_bRun{ false };
	bool m_bWorkPending; // sticky wake flag, protected by m_mutex

	wxMutex m_mutex;
	wxCondition m_condition;

	std::list<MediaProbeJob> m_jobList; // protected by m_mutex
};

#endif // MEDIAPROBETHREAD_H
