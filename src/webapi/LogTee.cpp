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

#include "LogTee.h"

#include <cerrno>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace webapi
{

namespace
{

// Thin platform layer over the descriptor primitives: POSIX on one side, the
// Windows CRT _-prefixed equivalents on the other. Windows lacks poll() on pipe
// descriptors, which is why the tee uses one blocking-read thread per stream
// rather than a single poll() loop.
#ifdef _WIN32
int OsPipe(int fds[2])
{
	return _pipe(fds, 1 << 16, _O_BINARY | _O_NOINHERIT);
}
int OsDup(int fd)
{
	return _dup(fd);
}
int OsDup2(int oldFd, int newFd)
{
	return _dup2(oldFd, newFd);
}
int OsClose(int fd)
{
	return _close(fd);
}
long OsRead(int fd, void *buf, unsigned n)
{
	return _read(fd, buf, n);
}
long OsWrite(int fd, const void *buf, unsigned n)
{
	return _write(fd, buf, n);
}
int StdoutFd()
{
	return _fileno(stdout);
}
int StderrFd()
{
	return _fileno(stderr);
}
bool RetryErrno()
{
	return false;
} // no EINTR on Windows
#else
int OsPipe(int fds[2])
{
	return ::pipe(fds);
}
int OsDup(int fd)
{
	return ::dup(fd);
}
int OsDup2(int oldFd, int newFd)
{
	return ::dup2(oldFd, newFd);
}
int OsClose(int fd)
{
	return ::close(fd);
}
long OsRead(int fd, void *buf, unsigned n)
{
	return ::read(fd, buf, n);
}
long OsWrite(int fd, const void *buf, unsigned n)
{
	return ::write(fd, buf, n);
}
int StdoutFd()
{
	return STDOUT_FILENO;
}
int StderrFd()
{
	return STDERR_FILENO;
}
bool RetryErrno()
{
	return errno == EINTR;
}
#endif

// close() a descriptor and reset the holder to -1.
void CloseFd(int &fd)
{
	if (fd >= 0) {
		OsClose(fd);
		fd = -1;
	}
}

// Write the whole buffer, retrying short writes (and EINTR on POSIX). Best
// effort: a hard error (e.g. the console fd went away) just stops this chunk.
void WriteAll(int fd, const char *buf, std::size_t n)
{
	std::size_t off = 0;
	while (off < n) {
		const long w = OsWrite(fd, buf + off, static_cast<unsigned>(n - off));
		if (w > 0) {
			off += static_cast<std::size_t>(w);
		} else if (w < 0 && RetryErrno()) {
			continue;
		} else {
			break;
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// CRotatingLog  (portable C stdio)
// ---------------------------------------------------------------------------

CRotatingLog::~CRotatingLog()
{
	Close();
}

bool CRotatingLog::Open(const std::string &path, std::size_t maxBytes)
{
	std::lock_guard<std::mutex> lk(m_mx);
	if (m_fp != nullptr) {
		return false;
	}
	std::FILE *fp = std::fopen(path.c_str(), "ab");
	if (fp == nullptr) {
		return false;
	}
	// Seed the running size from the existing file so the cap accounts for
	// content written in earlier runs (append mode).
	std::fseek(fp, 0, SEEK_END);
	const long pos = std::ftell(fp);
	m_curSize = pos > 0 ? static_cast<std::size_t>(pos) : 0;
	m_path = path;
	m_maxBytes = maxBytes;
	m_fp = fp;
	return true;
}

void CRotatingLog::Rotate()
{
	// m_mx held by caller.
	std::fclose(m_fp);
	m_fp = nullptr;
	const std::string rotated = m_path + ".1";
	// rename() replaces any previous ".1"; ignore failure and still try to
	// reopen so logging continues even if the rename could not happen.
	std::rename(m_path.c_str(), rotated.c_str());
	m_fp = std::fopen(m_path.c_str(), "wb");
	m_curSize = 0;
}

void CRotatingLog::Write(const char *buf, std::size_t n)
{
	std::lock_guard<std::mutex> lk(m_mx);
	if (m_fp == nullptr || n == 0) {
		return;
	}
	// Rotate before writing when the cap would be crossed, but never on an empty
	// file (a single chunk larger than the cap still has to go somewhere).
	if (m_maxBytes > 0 && m_curSize > 0 && m_curSize + n > m_maxBytes) {
		Rotate();
		if (m_fp == nullptr) {
			return;
		}
	}
	std::fwrite(buf, 1, n, m_fp);
	std::fflush(m_fp); // push to the OS so a crash doesn't lose the tail
	m_curSize += n;
}

void CRotatingLog::Close()
{
	std::lock_guard<std::mutex> lk(m_mx);
	if (m_fp != nullptr) {
		std::fclose(m_fp);
		m_fp = nullptr;
	}
}

int CRotatingLog::Fd() const
{
	std::lock_guard<std::mutex> lk(m_mx);
#ifdef _WIN32
	return m_fp != nullptr ? _fileno(m_fp) : -1;
#else
	return m_fp != nullptr ? fileno(m_fp) : -1;
#endif
}

// ---------------------------------------------------------------------------
// CLogTee
// ---------------------------------------------------------------------------

CLogTee::~CLogTee()
{
	Uninstall();
}

bool CLogTee::Install(const std::string &logPath, std::size_t maxBytes)
{
	if (m_installed) {
		return false;
	}
	if (!m_log.Open(logPath, maxBytes)) {
		return false;
	}

	// Preserve the real console so the pumps can echo to it.
	m_savedOut = OsDup(StdoutFd());
	m_savedErr = OsDup(StderrFd());
	if (m_savedOut < 0 || m_savedErr < 0) {
		CloseFd(m_savedOut);
		CloseFd(m_savedErr);
		m_log.Close();
		return false;
	}

	int pout[2] = { -1, -1 };
	int perr[2] = { -1, -1 };
	if (OsPipe(pout) != 0 || OsPipe(perr) != 0) {
		CloseFd(pout[0]);
		CloseFd(pout[1]);
		CloseFd(m_savedOut);
		CloseFd(m_savedErr);
		m_log.Close();
		return false;
	}

	// Route fd 1 and 2 into the pipe write ends.
	if (OsDup2(pout[1], StdoutFd()) < 0 || OsDup2(perr[1], StderrFd()) < 0) {
		OsDup2(m_savedOut, StdoutFd());
		OsDup2(m_savedErr, StderrFd());
		CloseFd(pout[0]);
		CloseFd(pout[1]);
		CloseFd(perr[0]);
		CloseFd(perr[1]);
		CloseFd(m_savedOut);
		CloseFd(m_savedErr);
		m_log.Close();
		return false;
	}
	// The write ends now live on fd 1/2; drop the spare copies.
	CloseFd(pout[1]);
	CloseFd(perr[1]);
	m_pipeOutRead = pout[0];
	m_pipeErrRead = perr[0];

	// On a TTY stdout was line-buffered; once fd 1 is a pipe libc switches it to
	// full (block) buffering, which would delay console + file output until 4-8
	// KB accumulate. Force line buffering back. stderr stays unbuffered.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);

	m_installed = true;
	m_outThread = std::thread(&CLogTee::Pump, this, m_pipeOutRead, m_savedOut);
	m_errThread = std::thread(&CLogTee::Pump, this, m_pipeErrRead, m_savedErr);
	return true;
}

void CLogTee::Pump(int readFd, int consoleFd)
{
	char buf[4096];
	for (;;) {
		const long n = OsRead(readFd, buf, sizeof(buf));
		if (n > 0) {
			// Console first so the terminal keeps behaving as it did, then the
			// file copy (locked + rotated inside CRotatingLog).
			WriteAll(consoleFd, buf, static_cast<std::size_t>(n));
			m_log.Write(buf, static_cast<std::size_t>(n));
		} else if (n == 0) {
			break; // write end closed
		} else if (RetryErrno()) {
			continue;
		} else {
			break;
		}
	}
}

void CLogTee::RedirectStderrToFileForCrash()
{
	const int fd = m_log.Fd();
	if (fd >= 0) {
		OsDup2(fd, StderrFd());
	}
}

void CLogTee::Uninstall()
{
	if (!m_installed) {
		return;
	}
	// Flush libc's stdout buffer into the pipe before we tear it down.
	std::fflush(stdout);
	std::fflush(stderr);

	// Restoring the console fds closes the pipe write ends that lived on fd 1/2,
	// so the pumps read EOF once they have drained what is still buffered.
	if (m_savedOut >= 0) {
		OsDup2(m_savedOut, StdoutFd());
	}
	if (m_savedErr >= 0) {
		OsDup2(m_savedErr, StderrFd());
	}

	if (m_outThread.joinable()) {
		m_outThread.join();
	}
	if (m_errThread.joinable()) {
		m_errThread.join();
	}

	CloseFd(m_pipeOutRead);
	CloseFd(m_pipeErrRead);
	CloseFd(m_savedOut);
	CloseFd(m_savedErr);
	m_log.Close();
	m_installed = false;
}

} // namespace webapi
