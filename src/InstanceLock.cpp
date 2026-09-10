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

#include <climits>
#include <cstdio>
#include <cstdlib>

#ifdef __WINDOWS__
#include <wx/filefn.h>
#include <wx/snglinst.h>
#include <wx/utils.h>
#else
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{

// Layout of the holder record, shared by both implementations:
//   line 1  the writer's pid
//   line 2  what it was: "amule", "amuled" or "amulegui"
// A file with only the first line was written by an aMule that predates the
// second, and leaves `kind` empty, which every caller reads as "assume it can
// be raised" -- the behaviour that shipped before.
void ParseHolder(const char *text, int &pid, wxString &kind)
{
	pid = 0;
	kind.clear();
	if (text == nullptr) {
		return;
	}
	char *rest = nullptr;
	const long parsed = strtol(text, &rest, 10);
	if (parsed > 0 && parsed <= INT_MAX) {
		pid = static_cast<int>(parsed);
	}
	if (rest != nullptr) {
		kind = wxString::FromUTF8(rest).Strip(wxString::both);
	}
}

#ifdef __WINDOWS__

void ReadHolderFile(const wxString &path, int &pid, wxString &kind)
{
	pid = 0;
	kind.clear();
	FILE *f = wxFopen(path, wxT("rb"));
	if (f == nullptr) {
		return;
	}
	char buf[96] = { 0 };
	const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	(void)fclose(f);
	buf[got] = '\0';
	ParseHolder(buf, pid, kind);
}

void WriteHolderFile(const wxString &path, const wxString &kind)
{
	FILE *f = wxFopen(path, wxT("wb"));
	if (f == nullptr) {
		return;
	}
	(void)fprintf(f, "%d\n%s\n", (int)wxGetProcessId(), (const char *)kind.utf8_str());
	(void)fclose(f);
}

#endif // __WINDOWS__

} // namespace

InstanceLock::InstanceLock()
#ifdef __WINDOWS__
: m_wxImpl(NULL)
, m_anotherRunning(false)
, m_holderPid(0)
#else
: m_fd(-1)
, m_holderPid(0)
#endif
{
}

InstanceLock::~InstanceLock()
{
	Release();
}

#ifdef __WINDOWS__

InstanceLock::Result InstanceLock::Acquire(
	const wxString &filename, const wxString &dir, const wxString &selfKind)
{
	Release();
	m_path = dir + filename;
	m_holderPid = 0;
	m_holderKind.clear();

	m_wxImpl = new wxSingleInstanceChecker();
	if (!m_wxImpl->Create(filename, dir)) {
		delete m_wxImpl;
		m_wxImpl = NULL;
		return LOCK_ERROR;
	}
	m_anotherRunning = m_wxImpl->IsAnotherRunning();

	// The mutex answers "is another instance running" but says nothing about
	// WHICH: amuled shares this name with the monolithic GUI, and only one of
	// them has a window to raise. wxSingleInstanceChecker keeps no file, so
	// the same two facts POSIX writes into the lock file are written beside
	// the mutex here, for the same reason and in the same format.
	//
	// No liveness check is needed on this side: the OS releases a named mutex
	// when its owner dies, so a held mutex is proof of a live holder, and a
	// file left behind by a crash is overwritten by the next acquire -- which
	// by definition is the process that just won the mutex.
	if (m_anotherRunning) {
		ReadHolderFile(m_path, m_holderPid, m_holderKind);
		return LOCK_HELD;
	}
	WriteHolderFile(m_path, selfKind);
	return LOCK_ACQUIRED;
}

void InstanceLock::Release()
{
	delete m_wxImpl;
	m_wxImpl = NULL;
	m_anotherRunning = false;
	if (!m_path.IsEmpty()) {
		(void)wxRemoveFile(m_path);
	}
}

#else // POSIX

InstanceLock::Result InstanceLock::Acquire(
	const wxString &filename, const wxString &dir, const wxString &selfKind)
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
		// EACCES and EAGAIN both mean "prohibited by another process's
		// lock" -- POSIX allows either and the platforms differ, so
		// neither can be read as a permission problem. Record who the
		// file SAYS owns it before closing, while we still have the fd.
		// Whether that pid is alive is the caller's question, and the
		// only thing separating "an aMule is running" from "something
		// else has this file".
		m_holderPid = 0;
		if (saved == EAGAIN || saved == EACCES) {
			char held[64] = { 0 };
			if (lseek(m_fd, 0, SEEK_SET) == 0) {
				ssize_t got = read(m_fd, held, sizeof(held) - 1);
				if (got > 0) {
					held[got] = '\0';
					ParseHolder(held, m_holderPid, m_holderKind);
				}
			}
		}
		(void)close(m_fd);
		m_fd = -1;
		if (saved == EAGAIN || saved == EACCES) {
			return LOCK_HELD;
		}
		return LOCK_ERROR;
	}

	// Refresh the on-disk PID, and what we are, as a diagnostic aid ("who
	// owns muleLock?" via `cat`) and so a later launch can tell a raisable
	// GUI holder from a headless daemon.
	// via `cat`). The kernel - not this integer - is the source of
	// truth. Under a PID-namespaced sandbox (Flatpak, docker) this is
	// the sandbox-local pid and may not match anything visible on the
	// host, but is still useful for triage.
	(void)ftruncate(m_fd, 0);
	char buf[96];
	int n = snprintf(buf, sizeof(buf), "%d\n%s\n", (int)getpid(), (const char *)selfKind.utf8_str());
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
