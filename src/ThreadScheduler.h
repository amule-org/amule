//
// This file is part of the aMule Project.
//
// Copyright (c) 2006-2011 Mikkel Schubert ( xaignar@amule.org / http://www.amule.org )
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

#ifndef THREADSCHEDULER_H
#define THREADSCHEDULER_H

#include <deque>
#include <map>

#include "Types.h"
#include "MuleThread.h"

class CThreadTask;

//! The priority values of tasks.
enum ETaskPriority
{
	ETP_Low = 0,
	ETP_Normal,
	ETP_High,
	//! For tasks such as finding shared files and ipfilter.dat loading only.
	ETP_Critical
};

/**
 * Manages the scheduling of background tasks.
 *
 * All tasks are assumed to be IO intensive, so only one runs at a time, and every thread runs at
 * lowest priority. Tasks are sorted by priority (see ETaskPriority) and age.
 *
 * The scheduler starts suspended, queueing tasks without executing them; call Start() to begin.
 */
class CThreadScheduler
{
public:
	/** Starts execution of queued tasks. */
	static void Start();

	/**
	 * Terminates task execution and frees the scheduler object. Tasks added afterwards are
	 * discarded.
	 */
	static void Terminate();

	/**
	 * Adds a new task to the queue, taking ownership of it, and returns true if it was queued.
	 *
	 * Before queueing, the task is checked against the existing ones by type and description; a
	 * matching task already present discards this object. It is also discarded if the scheduler
	 * has been terminated. With `overwrite`, any existing duplicate is dropped, and terminated
	 * if already running.
	 *
	 * @see Start
	 * @see Terminate
	 */
	static bool AddTask(CThreadTask *task, bool overwrite = false);

	/**
	 * The number of tasks still to be completed: those waiting on the queue, plus the one being
	 * executed.
	 *
	 * For progress reporting, so a caller that has queued a batch can tell how much of it is
	 * left without tracking completions itself. This is a snapshot taken under the scheduler's
	 * lock; the worker may have moved on by the time it is read, so treat it as a lower bound
	 * rather than an exact figure.
	 *
	 * @param type Count only tasks of this type, as passed to the CThreadTask constructor;
	 * empty counts every task. Callers reporting on a batch of their own want the filter: the
	 * queue is shared, and other subsystems add to it.
	 */
	static size_t GetPendingCount(const wxString &type = wxEmptyString);

private:
	CThreadScheduler();
	~CThreadScheduler();

	/** Returns the number of tasks on the queue. */
	size_t GetTaskCount() const;

	/** Tries to add the given task to the queue, returning true on success. */
	bool DoAddTask(CThreadTask *task, bool overwrite);

	/** Creates the actual scheduler thread if none exist. */
	void CreateSchedulerThread();

	/** Entry function called via internal thread-object. */
	void *Entry();

	//! Contains a task and its age.
	typedef std::pair<CThreadTask *, uint32> CEntryPair;

	//! List of currently scheduled tasks.
	std::deque<CEntryPair> m_tasks;

	//! Specifies if tasks should be resorted by priority.
	bool m_tasksDirty;

	typedef std::map<wxString, CThreadTask *> CDescMap;
	typedef std::map<wxString, CDescMap> CTypeMap;
	//! Map of current task by type -> desc. Used to avoid duplicate tasks.
	CTypeMap m_taskDescs;

	//! The actual worker thread.
	CMuleThread *m_thread;
	//! The currently running task, if any.
	CThreadTask *m_currentTask;

	friend class CTaskThread;
	friend struct CTaskSorter;
};

/**
 * Base class of all threaded tasks.
 *
 * Acts as a pseudo-thread, transparently executed on a worker thread by CThreadScheduler. The task
 * type should be a unique description of the KIND of task, since it is used to detect completion of
 * all tasks of a given type and, with the description, to find duplicates. The description should
 * be unique for the given task.
 */
class CThreadTask
{
public:
	/**
	 * @param type A name constant among tasks of this kind (hashing, completion, ...).
	 * @param desc A unique description for this task, for detecting duplicates.
	 * @param priority Decides how soon the task is carried out.
	 */
	CThreadTask(const wxString &type, const wxString &desc, ETaskPriority priority = ETP_Normal);

	/** Needed since CThreadScheduler only works with CThreadTask pointers. */
	virtual ~CThreadTask();

	/** Returns the task type, used for debugging and duplicate detection. */
	const wxString &GetType() const;

	/** Returns the task description, used for debugging and duplicate detection. */
	const wxString &GetDesc() const;

	/** Returns the priority of the task. Used when selecting the next task. */
	ETaskPriority GetPriority() const;

protected:
	//! @see wxThread::Entry
	virtual void Entry() = 0;

	/** Called when the last task of a specific type has been completed. */
	virtual void OnLastTask();

	/** @see wxThread::OnExit */
	virtual void OnExit();

	/** @see wxThread::TestDestroy */
	bool TestDestroy() const;

private:
	wxString m_type;
	wxString m_desc;
	ETaskPriority m_priority;

	//! The owner (scheduler), used when calling TestDestroy.
	CMuleThread *m_owner;
	//! Specifies if the specific task should be aborted.
	bool m_abort;

	friend class CThreadScheduler;
};

#endif
// File_checked_for_headers
