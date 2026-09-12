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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef WEBAPI_LOG_TEE_H
#define WEBAPI_LOG_TEE_H

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace webapi
{

// Append-only log file with a simple two-file size cap. When a write would cross maxBytes the
// current file is renamed to "<path>.1" (replacing any older rotation) and a fresh file is opened,
// so on-disk usage stays bounded to ~2x maxBytes while the most recent history is always preserved.
//
// Portable (C stdio), no file descriptors 1/2 and no threads -- this is the sink CLogTee writes
// into, and it is unit-tested directly. All public methods are internally locked, so the two
// forwarding threads may call Write() concurrently.
class CRotatingLog
{
public:
	CRotatingLog() = default;
	~CRotatingLog();

	CRotatingLog(const CRotatingLog &) = delete;
	CRotatingLog &operator=(const CRotatingLog &) = delete;

	// Opens path in append mode and seeds the running size from the existing file, so the cap
	// accounts for pre-existing content. maxBytes == 0 disables rotation. False on open
	// failure.
	bool Open(const std::string &path, std::size_t maxBytes);

	// Appends n bytes, rotating first if the cap would be crossed. No-op when
	// not open.
	void Write(const char *buf, std::size_t n);

	void Close();

	// A descriptor for the crash path to write to, or -1 when there is none.
	//
	// On POSIX it is reserved: one number for the life of the object, only ever re-pointed, so a
	// signal handler can read it without taking m_mx and the number can never be freed
	// mid-rotation and handed to another thread's socket. Close() leaves it open on the last
	// file, so the number is never a closed one in a reader's hands.
	//
	// Windows reserves nothing, because an open handle there blocks the rename() that rotation
	// needs. It resolves the current file under m_mx instead, which is what this did before the
	// reserved descriptor existed. The lock is a hazard in a handler, but the only Windows caller
	// is CLogTee::RedirectStderrToFileForCrash() from an SEH filter, and losing the redirect
	// altogether is worse: the report would sit in a pipe whose pump dies with the process.
	int CrashFd() const;

private:
	void Rotate();       // caller holds m_mx
	void PointCrashFd(); // caller holds m_mx

	mutable std::mutex m_mx;
	std::string m_path;
	std::size_t m_maxBytes = 0;
	std::size_t m_curSize = 0;
	std::FILE *m_fp = nullptr;
	volatile std::sig_atomic_t m_crashFd = -1;
};

// Duplicates the process's stdout and stderr into a log file while leaving the original console
// streams intact (a "tee"). It works at the file-descriptor level -- fd 1 and fd 2 are routed
// through pipes and a forwarding thread copies each chunk to both the saved console fd and the log
// file -- so it captures C stdio, C++ streams and anything else that writes to those descriptors,
// including the fatal-signal backtrace. Cross-platform via the POSIX and Windows pipe/dup2/read
// equivalents.
class CLogTee
{
public:
	CLogTee() = default;
	~CLogTee();

	CLogTee(const CLogTee &) = delete;
	CLogTee &operator=(const CLogTee &) = delete;

	// Opens logPath (append, capped at maxBytes), redirects fd 1 and 2 through pipes and starts
	// the forwarding threads. On any failure it restores the descriptors and returns false,
	// leaving stdout/stderr untouched.
	bool Install(const std::string &logPath, std::size_t maxBytes);

	// Restores the original descriptors, drains and joins the forwarding
	// threads and closes the file. Idempotent; also called by the destructor.
	void Uninstall();

	// Crash path: point fd 2 straight at the log file so a backtrace from the fatal handler is
	// written synchronously, without depending on the forwarding thread being scheduled before
	// the process dies.
	void RedirectStderrToFileForCrash();

	// The reserved crash descriptor, or -1 when not installed. For a crash reporter that has to
	// write somewhere the forwarding threads are not needed to drain; see
	// SetFatalAbortRedirectFd(). Stays valid across rotations; clear the reporter's copy before
	// destroying this object, which is what closes it.
	int CrashFd() const { return m_installed ? m_log.CrashFd() : -1; }

	// The dup of the original fd 2, or -1 when not installed. A crash reporter writes the console
	// copy here rather than to fd 2: fd 2 is the tee pipe, which reaches the console only while
	// the pump thread is still scheduled, and in a signal handler it is not.
	int ConsoleFd() const { return m_installed ? m_savedErr : -1; }

	bool IsInstalled() const { return m_installed; }

private:
	// One forwarding worker per stream: blocking-reads a pipe and writes each chunk to its
	// console fd and the log file. Two threads rather than one poll() loop so the same code
	// runs on Windows, which cannot poll() pipes.
	void Pump(int readFd, int consoleFd);

	bool m_installed = false;
	CRotatingLog m_log;

	int m_savedOut = -1;    // dup of the original fd 1
	int m_savedErr = -1;    // dup of the original fd 2
	int m_pipeOutRead = -1; // read end for the stdout pipe
	int m_pipeErrRead = -1; // read end for the stderr pipe

	std::thread m_outThread;
	std::thread m_errThread;
};

} // namespace webapi

#endif // WEBAPI_LOG_TEE_H
