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
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
//

#ifndef INSTANCELOCK_H
#define INSTANCELOCK_H

#include <wx/string.h>

class wxSingleInstanceChecker;

// Cross-instance detector for aMule.
//
// Replaces wxSingleInstanceChecker on POSIX because wx's stale-lock
// recovery falls back to kill(pid, 0) for liveness, which false-negatives
// across Linux PID namespaces (Flatpak sandboxes, container runtimes):
// every sandbox has its own PID 1..N, so the PID written by the first
// instance always maps to a live process in the second's namespace, and
// IsAnotherRunning() ends up returning false because m_pidLocker == getpid()
// on both sides. Result: two aMule processes stomping on the same config
// and downloads directory.
//
// This class uses fcntl(F_SETLK) directly: the kernel - not a PID compare -
// owns the truth about who holds the lock, and it's namespace-independent.
// On process death (clean exit, crash, SIGKILL) the kernel releases the
// lock automatically, so a lock file left over from a crashed run simply
// self-heals on the next start.
//
// On Windows the wxSingleInstanceChecker path is retained: it uses a named
// mutex, which is already namespace-aware.

class InstanceLock
{
public:
	InstanceLock();
	~InstanceLock();

	enum Result
	{
		LOCK_ACQUIRED, // we own the lock; proceed with normal startup
		LOCK_HELD,     // another live instance holds the lock
		LOCK_ERROR     // could not open the lock file (bad path/perms)
	};

	// Try to acquire the lock. Idempotent: if a prior lock is held, the
	// underlying fd is closed first (kernel drops the old lock), so calling
	// twice on the same object is safe. This is what
	// CamuleAppCommon::RefreshSingleInstanceChecker() relies on after
	// daemon fork().
	// `selfKind` is recorded in the lock file beside the pid: "amule",
	// "amuled" or "amulegui". amuled shares muleLock with the monolithic
	// GUI, so a second launch that finds the lock held has to know whether
	// the holder has a window to raise. A daemon has none, and a raise
	// request posted to one is the same silence as no message at all.
	Result Acquire(const wxString &filename, const wxString &dir, const wxString &selfKind);

	// Release the lock and unlink the on-disk file. Also called from
	// the destructor.
	void Release();

	// The pid recorded inside the lock file, read when Acquire() returned
	// LOCK_HELD, or 0 when there was none to read. Diagnostic only: the
	// kernel owns the truth about who holds the lock, and this is used
	// solely to decide what to TELL the user, never to decide the lock.
	//
	// A live pid means an aMule is there to be raised. A dead one means the
	// holder is not the aMule that wrote the file, which is the case that
	// used to exit without a word.
	int HolderPid() const { return m_holderPid; }

	// What the holder recorded itself as, empty when the file predates this
	// or was written by something that is not aMule. Empty means "assume it
	// can be raised", which is the behaviour that shipped before.
	const wxString &HolderKind() const { return m_holderKind; }

	// Path of the lock file Acquire() last worked on, for the message.
	const wxString &Path() const { return m_path; }

private:
#ifdef __WINDOWS__
	wxSingleInstanceChecker *m_wxImpl;
	bool m_anotherRunning;
#else
	int m_fd;
#endif
	// Shared by both implementations: the descriptive file beside the lock,
	// and what the last Acquire() read out of it.
	wxString m_path;
	int m_holderPid;
	wxString m_holderKind;
};

#endif // INSTANCELOCK_H
