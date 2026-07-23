//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 1998 Vadim Zeitlin ( zeitlin@dptmaths.ens-cachan.fr )
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

#include "config.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#include <stdint.h> // uintptr_t for pointer arithmetic in the
		    // SIGBUS handler -- `unsigned long` is 4 bytes on
		    // LLP64 and would silently truncate pointers.
#endif

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#include "FileArea.h"      // Interface declarations.
#include "FileAutoClose.h" // Needed for CFileAutoClose

#include <atomic> // std::atomic<bool> for the cross-thread MMapEnabled toggle

// MMAP_SUPPORTED is set by cmake (glib21.cmake) only when the whole mmap
// file-I/O path is available on this platform: mmap / munmap / sysconf /
// _SC_PAGESIZE / sigaction all present.  It gates the code below, the
// preferences checkbox and the EC tag.  Whether mmap is actually *used* is the
// runtime MMapEnabled preference (s_mmapEnabled), which defaults to off.

// Runtime switch for the mmap path (the MMapEnabled preference). Written from
// the main thread when the preference changes, read from the block-receive and
// upload-I/O threads at ReadAt()/StartWriteAt() time. A plain atomic bool is
// sufficient: the decision is captured per CFileArea in m_mmap_buffer, so a
// concurrent flip only changes the mode of *subsequent* operations.
//
// The store uses release / the loads use acquire so that the SIGSEGV/SIGBUS
// handler installed inside SetMMapEnabled(true) (below, once the handler class
// is declared) is guaranteed visible to any I/O thread that observes the flag
// as enabled -- i.e. the handler is always up before the first faultable map.
static std::atomic<bool> s_mmapEnabled{ false };

bool CFileArea::GetMMapEnabled()
{
	return s_mmapEnabled.load(std::memory_order_acquire);
}

#ifdef MMAP_SUPPORTED

#include <sys/mman.h>

#if defined(HAVE_SYSCONF) && defined(HAVE__SC_PAGESIZE)
static const long gs_pageSize = sysconf(_SC_PAGESIZE);
#elif defined(HAVE_SYSCONF) && defined(HAVE__SC_PAGE_SIZE)
static const long gs_pageSize = sysconf(_SC_PAGE_SIZE);
#endif

#endif /* MMAP_SUPPORTED */

#if !defined(HAVE_SIGACTION) || !defined(SA_SIGINFO) || !defined(MMAP_SUPPORTED) || defined(__UCLIBC__)

class CFileAreaSigHandler
{
public:
	static void Init() {};
	static void Add(CFileArea &) {};
	static void Remove(CFileArea &) {};

private:
	CFileAreaSigHandler() {};
};

#else

class CFileAreaSigHandler
{
public:
	static void Init();
	static void Add(CFileArea &area);
	static void Remove(CFileArea &area);

private:
	CFileAreaSigHandler() {};
	static wxMutex mutex;
	static CFileArea *first;
	static bool initialized;
	static struct sigaction old_segv, old_bus;
	static void Handler(int sig, siginfo_t *info, void *ctx);
};

wxMutex CFileAreaSigHandler::mutex;
CFileArea *CFileAreaSigHandler::first;
bool CFileAreaSigHandler::initialized = false;
struct sigaction CFileAreaSigHandler::old_segv;
struct sigaction CFileAreaSigHandler::old_bus;

/* define MAP_ANONYMOUS for Mac OS X */
#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

// Handle signals.
// The idea is to replace faulted memory with zeroes and mark
// the error in proper CFileArea
void CFileAreaSigHandler::Handler(int sig, siginfo_t *info, void *ctx)
{
	CFileArea *cur;
	// find the mapped section where violation occurred (if any)
	{
		wxMutexLocker lock(mutex);
		cur = first;
		while (cur) {
			if (cur->m_mmap_buffer && info->si_addr >= cur->m_mmap_buffer &&
				info->si_addr < cur->m_mmap_buffer + cur->m_length)
				break;
			cur = cur->m_next;
		}
	}

	// mark error if found
	if (cur && gs_pageSize > 0) {
		cur->m_error = true;
		char *start_addr = ((char *)info->si_addr) - (((uintptr_t)info->si_addr) % gs_pageSize);
		if (mmap(start_addr,
			    gs_pageSize,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
			    -1,
			    0) != MAP_FAILED)
			return;
	}

	// call old handler
	struct sigaction *sa = (sig == SIGSEGV) ? &old_segv : &old_bus;
	if (sa->sa_flags & SA_SIGINFO)
		sa->sa_sigaction(sig, info, ctx);
	else if (sa->sa_handler == SIG_DFL || sa->sa_handler == SIG_IGN)
		abort();
	else
		sa->sa_handler(sig);
}

void CFileAreaSigHandler::Init()
{
	// init error handler if needed
	wxMutexLocker lock(mutex);
	if (initialized)
		return;

	// Set our new signal handler.
	// Note that we safe old handlers (probably wx ones) in order
	// to be able to call them if signal not handled as desired.
	// These handler will be removed by wx code when wx will restore
	// old ones
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = Handler;
	sa.sa_flags = SA_NODEFER | SA_SIGINFO;
	if (sigaction(SIGSEGV, &sa, &old_segv))
		return;
	if (sigaction(SIGBUS, &sa, &old_bus)) {
		sigaction(SIGSEGV, &old_segv, NULL);
		return;
	}
	initialized = true;
}

void CFileAreaSigHandler::Add(CFileArea &area)
{
	wxMutexLocker lock(mutex);
	area.m_next = first;
	first = &area;
}

void CFileAreaSigHandler::Remove(CFileArea &area)
{
	wxMutexLocker lock(mutex);
	CFileArea **cur = &first;
	while (*cur) {
		if (*cur == &area) {
			*cur = area.m_next;
			area.m_next = NULL;
			break;
		}
		cur = &(*cur)->m_next;
	}
}
#endif

void CFileArea::SetMMapEnabled(bool enabled)
{
	// Install the SIGSEGV/SIGBUS recovery handler once, when mmap is first
	// turned on, rather than per read/write (which would take a mutex on every
	// block). Off builds -- and runs that never enable mmap -- never touch the
	// signal-handler chain, keeping them clear of sanitizers/debuggers/crash
	// reporters. CFileAreaSigHandler::Init() is idempotent and a no-op where
	// MMAP_SUPPORTED is not defined. The install happens-before the release
	// store, so any I/O thread that later sees the flag set (acquire) is
	// guaranteed the handler is already in place.
	if (enabled) {
		CFileAreaSigHandler::Init();
	}
	s_mmapEnabled.store(enabled, std::memory_order_release);
}

CFileArea::CFileArea()
: m_buffer(NULL)
, m_mmap_buffer(NULL)
, m_length(0)
, m_next(NULL)
, m_file(NULL)
, m_error(false)
{
	// The SIGSEGV/SIGBUS recovery handler is installed lazily, at the first
	// actual mmap() (see ReadAt/StartWriteAt), rather than here. That keeps
	// builds running with MMapEnabled=off (the default) from ever touching the
	// signal-handler chain, so they don't collide with sanitizers, debuggers
	// or crash reporters.
}

CFileArea::~CFileArea()
{
	Close();
	CheckError();
}

bool CFileArea::Close()
{
	if (m_buffer != NULL && m_mmap_buffer == NULL) {
		delete[] m_buffer;
		m_buffer = NULL;
	}
#ifdef MMAP_SUPPORTED
	if (m_mmap_buffer) {
		munmap(m_mmap_buffer, m_length);
		// remove from list
		CFileAreaSigHandler::Remove(*this);
		m_buffer = NULL;
		m_mmap_buffer = NULL;
		if (m_file) {
			m_file->Unlock();
			m_file = NULL;
		}
	}
#endif
	return true;
}

void CFileArea::ReadAt(CFileAutoClose &file, uint64 offset, size_t count)
{
	Close();

#ifdef MMAP_SUPPORTED
	uint64 offEnd = offset + count;
	// The 4 GiB cap only matters where off_t (the mmap offset argument) is
	// 32 bits: there a region ending at/after 4 GiB would truncate the offset
	// and map the wrong bytes (forum 16444). Where off_t is 64 bits -- every
	// modern aMule build (_FILE_OFFSET_BITS=64) -- mmap handles large offsets
	// natively (verified against >4 GiB part files), so the cap is skipped and
	// large files get mmap for their whole length. sizeof() folds at compile
	// time, so this is free on 64-bit.
	if (s_mmapEnabled.load(std::memory_order_acquire) && gs_pageSize > 0 &&
		(sizeof(off_t) >= 8 || offEnd < 0x100000000ull)) {
		uint64 offStart = offset & (~((uint64)gs_pageSize - 1));
		m_length = offEnd - offStart;
		void *p = mmap(NULL, m_length, PROT_READ, MAP_SHARED, file.fd(), offStart);
		if (p != MAP_FAILED) {
			m_file = &file;
			m_mmap_buffer = (uint8_t *)p;
			m_buffer = m_mmap_buffer + (offset - offStart);

			// add to list to catch errors correctly
			CFileAreaSigHandler::Add(*this);
			return;
		}
	}
	file.Unlock();
#endif
	m_buffer = new uint8_t[count];
	file.ReadAt(m_buffer, offset, count);
}

#ifdef MMAP_SUPPORTED
void CFileArea::StartWriteAt(CFileAutoClose &file, uint64 offset, size_t count)
{
	Close();

	uint64 offEnd = offset + count;
	// 4 GiB cap only applies with a 32-bit off_t -- see ReadAt() above.
	if (s_mmapEnabled.load(std::memory_order_acquire) && file.GetLength() >= offEnd && gs_pageSize > 0 &&
		(sizeof(off_t) >= 8 || offEnd < 0x100000000ull)) {
		uint64 offStart = offset & (~((uint64)gs_pageSize - 1));
		m_length = offEnd - offStart;
		void *p = mmap(NULL, m_length, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd(), offStart);
		if (p != MAP_FAILED) {
			m_file = &file;
			m_mmap_buffer = (uint8_t *)p;
			m_buffer = m_mmap_buffer + (offset - offStart);

			// add to list to catch errors correctly
			CFileAreaSigHandler::Add(*this);
			return;
		}
		file.Unlock();
	}
	m_buffer = new uint8_t[count];
}
#else
void CFileArea::StartWriteAt(CFileAutoClose &, uint64, size_t count)
{
	Close();
	m_buffer = new uint8_t[count];
}
#endif

bool CFileArea::FlushAt(CFileAutoClose &file, uint64 offset, size_t count)
{
	if (!m_buffer)
		return false;

#ifdef MMAP_SUPPORTED
	if (m_mmap_buffer) {
		if (msync(m_mmap_buffer, m_length, MS_SYNC))
			return false;
		Close();
		return true;
	}
#endif
	file.WriteAt(m_buffer, offset, count);
	Close();
	return true;
}

void CFileArea::CheckError()
{
	bool err = m_error;
	m_error = false;
	if (err)
		throw CIOFailureException("Read error, failed to read from file.");
}
