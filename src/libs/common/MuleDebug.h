//
// This file is part of the aMule Project.
//
// Copyright (c) 2005-2011 Mikkel Schubert ( xaignar@users.sourceforge.net )
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

#ifndef MULEDEBUG_H
#define MULEDEBUG_H

#include <wx/string.h>

#include <cstdio>

/**
 * Installs an exception handler that can handle CMuleExceptions.
 */
void InstallMuleExceptionHandler();

void OnUnhandledException();

//! Print a backtrace, skipping the first n frames.
void print_backtrace(unsigned n);

//! Returns a backtrace, skipping the first n frames.
wxString get_backtrace(unsigned n);

/**
 * Arms a SIGABRT handler that writes a raw backtrace to stderr.
 *
 * wx's fatal-exception support covers SIGSEGV, SIGBUS, SIGILL and SIGFPE, so a glibc heap abort
 * ("double free or corruption", "free(): invalid pointer") bypasses the whole reporting path and
 * the process dies silently. That is what left amule-org/amule#1338 with no backtrace at all.
 *
 * Deliberately NOT get_backtrace(): we are entered from abort(), which glibc calls from
 * malloc_printerr with the allocator's state already inconsistent and, above the tcache ceiling,
 * its arena lock held. get_backtrace() allocates throughout -- backtrace_symbols(), std::vector,
 * wxString -- and on the non-BFD path it popen()s addr2line. So this writes frames with
 * backtrace_symbols_fd(), the variant documented not to allocate. They go to the descriptor set by
 * SetFatalAbortRedirectFd(), or to STDERR_FILENO when none is set.
 *
 * Worth being exact about the risk, because it is a guarantee rather than an observed failure: an
 * allocating handler does NOT reliably hang. Measured on glibc 2.43, a double free above the
 * tcache ceiling reaches malloc_printerr with the arena lock held, yet a handler calling
 * backtrace_symbols() still completed -- its own allocation was small enough to come from tcache,
 * which needs no lock. The case against it is that this depends on a free chunk of the right size
 * happening to be cached, on a heap already known to be damaged, with fork() in the fallback path.
 * backtrace_symbols_fd() needs none of that to hold.
 *
 * The cost is raw output: module, offset and mangled symbol, no demangling and no line numbers.
 * Run addr2line over the addresses offline.
 *
 * Call once per process, after the wx fatal handlers are armed.
 */
void InstallFatalAbortHandler();

/**
 * Tells the SIGABRT handler to stay quiet for the next abort.
 *
 * For callers that have already reported: ReportAssertFailure() prints a full symbolicated
 * backtrace and then raise(SIGABRT)s to give gdb something to catch, and a second raw dump on top
 * of it is noise.
 */
void SuppressNextAbortBacktrace();

/**
 * Gives the crash reporters a durable descriptor to report to.
 *
 * For a process that has put something other than a terminal on fd 2. amuleapi tees stdout and
 * stderr through pipes that a forwarding thread drains, so a report written only to fd 2 leaves the
 * backtrace in a pipe whose reader dies with the process moments later.
 *
 * The reporters write here and to the console descriptor both, so a container keeps the report in
 * `docker logs` and a terminal still shows it. Pass -1 to go back to fd 2 alone.
 *
 * This covers the SIGABRT and std::terminate paths. It does NOT replace
 * CamuleapiApp::OnFatalException's call to CLogTee::RedirectStderrToFileForCrash(): wx's own
 * SIGSEGV/SIGBUS/SIGILL/SIGFPE handler reports through stderr, so without that dup2() the SIGSEGV
 * backtrace still dies in the tee pipe.
 */
void SetFatalAbortRedirectFd(int fd);

/**
 * Where the console copy of a crash report goes, when fd 2 is not it.
 *
 * amuleapi routes fd 2 through its tee pipe, so a report written there reaches the console only if
 * the pump thread is still scheduled -- true at std::terminate, not true in a signal handler.
 * CLogTee::ConsoleFd() hands over the dup of the original fd 2 it already keeps, and writing
 * straight to that reaches `docker logs` in both cases.
 *
 * Pass -1 (the default) to use fd 2. Clear it before the descriptor is closed, exactly as for
 * SetFatalAbortRedirectFd().
 */
void SetFatalAbortConsoleFd(int fd);

/**
 * A one-line build identifier for the raw SIGABRT banner, copied into a fixed buffer.
 *
 * The banner is written from a signal handler, so it cannot format anything: a report pasted from
 * the field would otherwise not say which build produced it. Call once the version is known.
 */
void SetFatalAbortVersionLine(const char *text);

/**
 * Re-points a reserved crash descriptor at the file behind @a fp, returning the reserved number.
 *
 * A fatal handler cannot look a descriptor up when it runs, because every path that resolves one
 * locks. It holds a single number instead, and this keeps that number pointing at the current
 * file: pass -1 the first time to reserve one, then pass the same value back after every reopen.
 * The number itself is never closed, so it cannot be freed mid-rotation and handed out to another
 * thread's socket while the handler still holds it.
 *
 * Returns @a reserved unchanged when @a fp is null or the dup fails.
 */
int ReserveCrashFd(int reserved, std::FILE *fp);

/**
 * Base for the other exception types. Never catch this; catch the subtypes.
 */
class CMuleException
{
public:
	CMuleException(const wxString &type, const wxString &desc)
	: m_what(type + ": " + desc)
	{
	}
	CMuleException(const CMuleException &) = default;
	CMuleException &operator=(const CMuleException &) = default;
	virtual ~CMuleException() noexcept {}
	virtual const wxString &what() const noexcept { return m_what; }

private:
	wxString m_what;
};

/**
 * Exceptions caused by invalid operations. Do not catch these -- they are the result of bugs.
 */
struct CRunTimeException : public CMuleException
{
	CRunTimeException(const wxString &type, const wxString &desc)
	: CMuleException("CRunTimeException::" + type, desc)
	{
	}
};

/**
 * Thrown if invalid parameters are passed to a function.
 */
struct CInvalidParamsEx : public CRunTimeException
{
	CInvalidParamsEx(const wxString &desc)
	: CRunTimeException("CInvalidArgsException", desc)
	{
	}
};

/**
 * Thrown if an object is used in an invalid state.
 */
struct CInvalidStateEx : public CRunTimeException
{
	CInvalidStateEx(const wxString &desc)
	: CRunTimeException("CInvalidStateException", desc)
	{
	}
};

/**
 * Thrown on wrong packets or tags.
 */
struct CInvalidPacket : public CMuleException
{
	CInvalidPacket(const wxString &desc)
	: CMuleException("CInvalidPacket", desc)
	{
	}
};

// This ifdef ensures that we wont get assertions while
// unittesting, which would otherwise impede the tests.
#ifdef MULEUNIT
#define _MULE_THROW(cond, cls, msg) \
	do { \
		if (!(cond)) { \
			throw cls(msg); \
		} \
	} while (false)
#else
#define _MULE_THROW(cond, cls, msg) \
	do { \
		if (!(cond)) { \
			wxFAIL_MSG(#cond); \
			throw cls(msg); \
		} \
	} while (false)
#endif

#define MULE_CHECK_THROW(cond, cls, msg) _MULE_THROW((cond), cls, (msg))

#define MULE_VALIDATE_STATE(cond, msg) MULE_CHECK_THROW((cond), CInvalidStateEx, (msg))

#define MULE_VALIDATE_PARAMS(cond, msg) MULE_CHECK_THROW((cond), CInvalidParamsEx, (msg))

#define MULE_ASSERT(cond) wxASSERT((cond))
#define MULE_ASSERT_MSG(cond, msg) wxASSERT_MSG((cond), msg)
#define MULE_FAIL() wxFAIL()
#define MULE_FAIL_MSG(msg) wxFAIL_MSG(msg)
#define MULE_CHECK(cond, retValue) wxCHECK((cond), (retValue))
#define MULE_CHECK_MSG(cond, ret, msg) wxCHECK_MSG((cond), (ret), (msg))
#define MULE_CHECK_RET(cond, msg) wxCHECK_RET((cond), (msg))

#endif
// File_checked_for_headers
