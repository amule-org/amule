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

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace webapi
{

// Append-only log file with a simple two-file size cap. When a write would
// cross maxBytes the current file is renamed to "<path>.1" (replacing any older
// rotation) and a fresh file is opened, so on-disk usage stays bounded to
// ~2x maxBytes while the most recent history is always preserved.
//
// Portable (C stdio), no file descriptors 1/2 and no threads -- this is the sink
// CLogTee writes into, and it is unit-tested directly. All public methods are
// internally locked, so the two forwarding threads may call Write() concurrently.
class CRotatingLog
{
public:
	CRotatingLog() = default;
	~CRotatingLog();

	CRotatingLog(const CRotatingLog &) = delete;
	CRotatingLog &operator=(const CRotatingLog &) = delete;

	// Opens path in append mode and seeds the running size from the existing
	// file so the cap accounts for pre-existing content. maxBytes == 0 disables
	// rotation (unbounded). Returns false on open failure.
	bool Open(const std::string &path, std::size_t maxBytes);

	// Appends n bytes, rotating first if the cap would be crossed. No-op when
	// not open.
	void Write(const char *buf, std::size_t n);

	void Close();

	// Underlying file descriptor of the current file, or -1. Used only by the
	// crash path to point stderr straight at the file; not for concurrent I/O.
	int Fd() const;

private:
	void Rotate(); // caller holds m_mx

	mutable std::mutex m_mx;
	std::string m_path;
	std::size_t m_maxBytes = 0;
	std::size_t m_curSize = 0;
	std::FILE *m_fp = nullptr;
};

// Duplicates the process's stdout and stderr into a log file while leaving the
// original console streams intact (a "tee"). It works at the file-descriptor
// level -- fd 1 and fd 2 are routed through pipes and a forwarding thread copies
// each chunk to both the saved console fd and the log file -- so it captures C
// stdio (printf/fprintf), C++ streams (std::cout/std::cerr) and anything else
// that writes to those descriptors, including the fatal-signal backtrace.
//
// Cross-platform: POSIX pipe/dup2/read and the Windows _pipe/_dup2/_read
// equivalents. amuleapi installs a single instance at startup; the other EC
// connectors do not use it.
class CLogTee
{
public:
	CLogTee() = default;
	~CLogTee();

	CLogTee(const CLogTee &) = delete;
	CLogTee &operator=(const CLogTee &) = delete;

	// Opens logPath (append, capped at maxBytes), redirects fd 1 and 2 through
	// pipes and starts the forwarding threads. On any failure it restores the
	// descriptors and returns false, leaving the process's stdout/stderr
	// untouched.
	bool Install(const std::string &logPath, std::size_t maxBytes);

	// Restores the original descriptors, drains and joins the forwarding
	// threads and closes the file. Idempotent; also called by the destructor.
	void Uninstall();

	// Crash path: point fd 2 straight at the log file so a backtrace emitted by
	// the fatal handler is written synchronously, without depending on the
	// forwarding thread being scheduled before the process dies.
	void RedirectStderrToFileForCrash();

	bool IsInstalled() const { return m_installed; }

private:
	// One forwarding worker per stream: blocking-reads a pipe and writes each
	// chunk to its console fd and the log file. Two threads (rather than one
	// poll() loop) so the same code runs on Windows, which cannot poll() pipe
	// descriptors.
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
