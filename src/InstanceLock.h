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
	Result Acquire(const wxString &filename, const wxString &dir);

	// Release the lock and unlink the on-disk file. Also called from
	// the destructor.
	void Release();

private:
#ifdef __WINDOWS__
	wxSingleInstanceChecker *m_wxImpl;
	bool m_anotherRunning;
#else
	int m_fd;
	wxString m_path;
#endif
};

#endif // INSTANCELOCK_H
