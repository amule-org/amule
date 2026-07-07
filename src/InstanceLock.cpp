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

#include "InstanceLock.h"

#ifdef __WINDOWS__
#include <wx/snglinst.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

InstanceLock::InstanceLock()
#ifdef __WINDOWS__
: m_wxImpl(NULL)
, m_anotherRunning(false)
#else
: m_fd(-1)
#endif
{
}

InstanceLock::~InstanceLock()
{
	Release();
}

#ifdef __WINDOWS__

InstanceLock::Result InstanceLock::Acquire(const wxString &filename, const wxString &dir)
{
	Release();
	m_wxImpl = new wxSingleInstanceChecker();
	if (!m_wxImpl->Create(filename, dir)) {
		delete m_wxImpl;
		m_wxImpl = NULL;
		return LOCK_ERROR;
	}
	m_anotherRunning = m_wxImpl->IsAnotherRunning();
	return m_anotherRunning ? LOCK_HELD : LOCK_ACQUIRED;
}

void InstanceLock::Release()
{
	delete m_wxImpl;
	m_wxImpl = NULL;
	m_anotherRunning = false;
}

#else // POSIX

InstanceLock::Result InstanceLock::Acquire(const wxString &filename, const wxString &dir)
{
	// Idempotent-caller contract (see header). Drop any existing fd first
	// without unlinking - the file will be replaced on the open() below;
	// unlink()-ing here would create a race window during which no lock
	// file exists on disk, and a concurrent third instance could slip in.
	if (m_fd != -1) {
		(void)close(m_fd);
		m_fd = -1;
	}
	m_path = dir + filename;

	// Open (or create) the lock file. Unlike wxSingleInstanceChecker we
	// do NOT use O_EXCL: the file's presence isn't the signal, the
	// kernel-held fcntl lock is.
	m_fd = open(m_path.fn_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (m_fd == -1) {
		return LOCK_ERROR;
	}

	// Defense against a planted lock file with wrong owner (matches
	// wxSingleInstanceChecker's paranoia). If someone dropped a
	// world-writable muleLock into ~/.aMule/ we don't want to touch it.
	struct stat st;
	if (fstat(m_fd, &st) == 0 && st.st_uid != getuid()) {
		(void)close(m_fd);
		m_fd = -1;
		return LOCK_ERROR;
	}
	(void)fchmod(m_fd, S_IRUSR | S_IWUSR);

	struct flock fl;
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	fl.l_pid = 0;

	if (fcntl(m_fd, F_SETLK, &fl) == -1) {
		int saved = errno;
		(void)close(m_fd);
		m_fd = -1;
		if (saved == EAGAIN || saved == EACCES) {
			return LOCK_HELD;
		}
		return LOCK_ERROR;
	}

	// Refresh the on-disk PID as a diagnostic aid ("who owns muleLock?"
	// via `cat`). The kernel - not this integer - is the source of
	// truth. Under a PID-namespaced sandbox (Flatpak, docker) this is
	// the sandbox-local pid and may not match anything visible on the
	// host, but is still useful for triage.
	(void)ftruncate(m_fd, 0);
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	if (n > 0) {
		ssize_t rc = write(m_fd, buf, (size_t)n);
		(void)rc;
	}
	return LOCK_ACQUIRED;
}

void InstanceLock::Release()
{
	if (m_fd == -1) {
		return;
	}
	// Unlink first so a fresh start doesn't inherit stale content;
	// close last so the fcntl lock is released atomically with removal.
	(void)unlink(m_path.fn_str());
	(void)close(m_fd);
	m_fd = -1;
	m_path.clear();
}

#endif // __WINDOWS__
