//
// This file is part of the aMule Project.
//
// Copyright (c) 2006-2011 Mikkel Schubert ( xaignar@amule.org / http:://www.amule.org )
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

#ifndef TASKS_H
#define TASKS_H

#include "ThreadScheduler.h"
#include "MediaProbe.h" // Needed for MediaInfo
#include "MD4Hash.h"    // Needed for CMD4Hash on CMediaProbeTask + CMediaProbeEvent
#include <common/Path.h>

class CKnownFile;
class CPartFile;
class CFileAutoClose;

/**
 * Performs MD4 and/or AICH hashing of a file, depending on the type.
 *
 * First constructor: a new shared file (part == NULL) gets both MD4 and AICH; an incomplete
 * partfile, rehashed because its timestamp changed, gets MD4 only; a complete partfile gets both.
 * Second constructor: an existing shared file gets an AICH hash only.
 *
 * @see CHashingEvent
 * @see CAICHSyncTask
 */
class CHashingTask : public CThreadTask
{
public:
	/**
	 * Schedules a partfile or new shared file for hashing.
	 *
	 * @param path The full path, without filename.
	 * @param filename The actual filename.
	 * @param part Identifies the owner in the event handler (PartFiles only).
	 *
	 * CHashingEvents from this kind of task have the id MULE_EVT_HASHING. @see EVT_MULE_HASHING
	 */
	CHashingTask(const CPath &path, const CPath &filename, const CPartFile *part = NULL);

	/**
	 * Schedules a KnownFile to have an AICH hashset created; used by CAICHSyncTask.
	 * CHashingEvents from this kind of task have the id MULE_EVT_AICH_HASHING. @see
	 * EVT_MULE_AICH_HASHING
	 */
	CHashingTask(const CKnownFile *toAICHHash);

protected:
	//! Which hashes to calculate when the task runs. EH_MD4 and EH_AICH are bit flags; the
	//! combined value is named explicitly so a bitwise OR cast cannot produce a value outside
	//! the enum's valid range.
	enum EHashes
	{
		EH_AICH = 1,
		EH_MD4 = 2,
		EH_MD4_AND_AICH = EH_MD4 | EH_AICH
	};

	virtual void OnLastTask();

	virtual void Entry();

	/**
	 * Hashes the next PARTSIZE chunk of @a file: an MD4 hash, plus an AICH hashset when @a
	 * toHash asks for one. @a part is the part number and @a owner the known (or part) file it
	 * belongs to.
	 *
	 * Returns false on read errors. Assumes it is never called for a closed or EOF file.
	 */
	bool CreateNextPartHash(CFileAutoClose &file, uint16 part, CKnownFile *owner, EHashes toHash);

	//! The path to the file to be hashed (shared or part), without filename.
	CPath m_path;
	//! The filename of the file to be hashed (filename only).
	CPath m_filename;
	//! Specifies which hash-types should be calculated
	EHashes m_toHash;
	//! If a partfile or an AICH hashing, this pointer stores it for callbacks.
	const CKnownFile *m_owner;

private:
	void SetHashingProgress(uint16 part);
};

// Media metadata probing (#140/#280) runs on the dedicated CMediaProbeThread, not the shared
// CThreadScheduler, so a slow or hung ffprobe cannot stall completions. Results still arrive via
// the CMediaProbeEvent below.

/**
 * Synchronizes the AICH hashlist: shared files lacking an AICH hash are scheduled for hashing.
 */
class CAICHSyncTask : public CThreadTask
{
public:
	/**
	 * @param pruneOrphans Rewrite known2_64.met dropping hashsets no longer referenced by any
	 * known file. Only safe when the known-file list is authoritative (startup); a post-hashing
	 * sync races the main-thread registration of the file it just hashed and would prune its
	 * own freshly-written hashset. Defaults to false so only the startup sync opts in.
	 */
	explicit CAICHSyncTask(bool pruneOrphans = false);

protected:
	/** See CThreadTask::Entry */
	virtual void Entry();

	/** Converts old known2.met files to known2_64.met files. */
	bool ConvertToKnown2ToKnown264();

private:
	bool m_pruneOrphans;
};

/**
 * Calculates MD4 and AICH hashes for a known file, to check file integrity against the hashes
 * stored in the .met files.
 */
class CVerifyLocalDataTask : public CThreadTask
{
public:
	explicit CVerifyLocalDataTask(const CMD4Hash &md4);

protected:
	/** See CThreadTask::Entry */
	virtual void Entry();

	std::vector<uint16> m_corruptedMD4;
	std::vector<std::pair<uint16, std::vector<uint8>>> m_corruptedAICH;
	CMD4Hash m_fileID;

private:
	void PrintReport(const CPath &fullPath, const bool checkedAICH);
};

/**
 * Performs the final steps on a complete download: finding a usable destination filename, removing
 * old data files and moving the part-file, possibly to a different partition.
 */
class CCompletionTask : public CThreadTask
{
public:
	/**
	 * Creates a thread which will complete the given download.
	 */
	CCompletionTask(const CPartFile *file);

protected:
	/** See CThreadTask::Entry */
	virtual void Entry();

	/** See CThreadTask::OnExit */
	virtual void OnExit();

	//! The target filename.
	CPath m_filename;
	//! The full path to the .met-file
	CPath m_metPath;
	//! The category of the download.
	uint8 m_category;
	//! Owner of the file, used when sending completion-event.
	const CPartFile *m_owner;
	//! Specifies if an error occurred during completion.
	bool m_error;
	//! The resulting full path. File may be be renamed.
	CPath m_newName;
};

/**
 * Preallocates space for a newly created partfile.
 */
class CAllocateFileTask : public CThreadTask
{
public:
	/** Creates a thread that will allocate disk space for the full file. */
	CAllocateFileTask(CPartFile *file, bool pause);

protected:
	/** See CThreadTask::Entry */
	virtual void Entry();

	/** See CThreadTask::OnExit */
	virtual void OnExit();

private:
	//! The partfile for which this task allocates space.
	CPartFile *m_file;

	//! Should this download start paused?
	bool m_pause;

	//! Result of the preallocation.
	long m_result;
};

/**
 * Signals the completion of a hashing event. @see CHashingTask
 */
class CHashingEvent : public wxEvent
{
public:
	/**
	 * @param type MULE_EVT_HASHING or MULE_EVT_AICH_HASHING.
	 */
	CHashingEvent(wxEventType type, CKnownFile *result, const CKnownFile *owner = NULL);

	/** @see wxEvent::Clone */
	virtual wxEvent *Clone() const;

	/** Returns the owner (may be NULL) of the hashing result. */
	const CKnownFile *GetOwner() const;
	/** Returns a CKnownfile used to store the results of the hashing. */
	CKnownFile *GetResult() const;

private:
	//! The file owner.
	const CKnownFile *m_owner;
	//! The hashing results.
	CKnownFile *m_result;
};

/**
 * Sent when a probe finished, whether or not it extracted anything. The main-thread handler
 * resolves the hash back to a CKnownFile* (via CKnownFileList::FindKnownFileByID) and attaches the
 * FT_MEDIA_* tags there -- doing that from the worker thread would race the publish paths that read
 * m_taglist.
 *
 * A FAILED probe is reported too (issue #1116): the handler records that the file was tried and
 * produced nothing, so the "already probed" gate stops re-queueing it on every reload and every
 * restart. Without that round trip a file ffprobe cannot read is indistinguishable from one never
 * probed.
 */
class CMediaProbeEvent : public wxEvent
{
public:
	// Carries the MediaInfo whole rather than one accessor per field: the struct already IS the
	// set of extracted fields, and mirroring them here meant every new field touched the event,
	// its ctor, its Clone and the handler. Strings are deep-copied on construction for the
	// thread hop -- see the ctor.
	//
	// succeeded == false carries no MediaInfo worth reading: the handler records the failure
	// and leaves whatever tags the file already had. markUnprobeable is meaningful only then --
	// it says the probe reached a verdict about the FILE (ffprobe ran and found nothing usable)
	// rather than failing on the environment (no binary, a timeout, a file that vanished). Only
	// the former may be recorded against the file.
	CMediaProbeEvent(const CMD4Hash &hash,
		const MediaInfo &info,
		bool succeeded = true,
		bool markUnprobeable = false);

	virtual wxEvent *Clone() const;

	const CMD4Hash &GetHash() const { return m_hash; }
	const MediaInfo &GetInfo() const { return m_info; }
	bool Succeeded() const { return m_succeeded; }
	bool MarkUnprobeable() const { return m_markUnprobeable; }

private:
	CMD4Hash m_hash;
	MediaInfo m_info;
	bool m_succeeded;
	bool m_markUnprobeable;
};

/**
 * Sent when a part-file has been completed.
 */
class CCompletionEvent : public wxEvent
{
public:
	/** Constructor, see getter funtion for description of parameters. */
	CCompletionEvent(bool errorOccured, const CPartFile *owner, const CPath &fullPath);

	/** @see wxEvent::Clone */
	virtual wxEvent *Clone() const;

	/** Returns true if completion failed. */
	bool ErrorOccurred() const;

	/** Returns the owner of the file that was being completed. */
	const CPartFile *GetOwner() const;

	/** Returns the full path to the completed file (empty on failure). */
	const CPath &GetFullPath() const;

private:
	//! The full path to the completed file.
	CPath m_fullPath;

	//! The owner of the completed .part file.
	const CPartFile *m_owner;

	//! Specifies if completion failed.
	bool m_error;
};

/**
 * Sent when preallocation of a new partfile is finished.
 */
wxDECLARE_EVENT(MULE_EVT_ALLOC_FINISHED, wxEvent);
class CAllocFinishedEvent : public wxEvent
{
public:
	/** Constructor, see getter function for description of parameters. */
	CAllocFinishedEvent(CPartFile *file, bool pause, long result)
	: wxEvent(-1, MULE_EVT_ALLOC_FINISHED)
	, m_file(file)
	, m_pause(pause)
	, m_result(result)
	{
	}

	/** @see wxEvent::Clone */
	virtual wxEvent *Clone() const;

	/** Returns the partfile for which preallocation was requested. */
	CPartFile *GetFile() const noexcept { return m_file; }

	/** Returns whether the partfile should start paused. */
	bool IsPaused() const noexcept { return m_pause; }

	/** Returns the result of preallocation: true on success, false otherwise. */
	bool Succeeded() const noexcept { return m_result == 0; }

	/** Returns the result of the preallocation. */
	long GetResult() const noexcept { return m_result; }

private:
	//! The partfile for which preallocation was requested.
	CPartFile *m_file;

	//! Should the download start paused?
	bool m_pause;

	//! Result of preallocation
	long m_result;
};

wxDECLARE_EVENT(MULE_EVT_HASHING, wxEvent);
wxDECLARE_EVENT(MULE_EVT_AICH_HASHING, wxEvent);
wxDECLARE_EVENT(MULE_EVT_FILE_COMPLETED, wxEvent);
wxDECLARE_EVENT(MULE_EVT_MEDIA_PROBE, wxEvent);

typedef void (wxEvtHandler::*MuleHashingEventFunction)(CHashingEvent &);
typedef void (wxEvtHandler::*MuleCompletionEventFunction)(CCompletionEvent &);
typedef void (wxEvtHandler::*MuleAllocFinishedEventFunction)(CAllocFinishedEvent &);
typedef void (wxEvtHandler::*MuleMediaProbeEventFunction)(CMediaProbeEvent &);

//! Event-handler for completed hashings of new shared files and partfiles.
#define EVT_MULE_HASHING(func) \
	wx__DECLARE_EVT0(MULE_EVT_HASHING, wxEVENT_HANDLER_CAST(MuleHashingEventFunction, func))

//! Event-handler for completed hashings of files that were missing a AICH hash.
#define EVT_MULE_AICH_HASHING(func) \
	wx__DECLARE_EVT0(MULE_EVT_AICH_HASHING, wxEVENT_HANDLER_CAST(MuleHashingEventFunction, func))

//! Event-handler for completion of part-files.
#define EVT_MULE_FILE_COMPLETED(func) \
	wx__DECLARE_EVT0(MULE_EVT_FILE_COMPLETED, wxEVENT_HANDLER_CAST(MuleCompletionEventFunction, func))

//! Event-handler for partfile preallocation finished events.
#define EVT_MULE_ALLOC_FINISHED(func) \
	wx__DECLARE_EVT0(MULE_EVT_ALLOC_FINISHED, wxEVENT_HANDLER_CAST(MuleAllocFinishedEventFunction, func))

//! Event-handler for MediaProbe-completed events.
#define EVT_MULE_MEDIA_PROBE(func) \
	wx__DECLARE_EVT0(MULE_EVT_MEDIA_PROBE, wxEVENT_HANDLER_CAST(MuleMediaProbeEventFunction, func))

#endif // TASKS_H
// File_checked_for_headers
