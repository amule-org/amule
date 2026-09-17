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
#include <cstring>
#include <ctime>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace webapi
{

namespace
{

// Thin platform layer over the descriptor primitives: POSIX on one side, the Windows CRT _-prefixed
// equivalents on the other. Windows lacks poll() on pipe descriptors, which is why the tee uses one
// blocking-read thread per stream rather than a single poll() loop.
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
int OsFileno(std::FILE *fp)
{
	return _fileno(fp);
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
int OsFileno(std::FILE *fp)
{
	return fileno(fp);
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

std::string LocalLogStamp()
{
	const std::time_t now = std::time(nullptr);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char buf[32];
	const std::size_t len = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S: ", &local);
	return std::string(buf, len);
}

CLineStamper::CLineStamper(Clock clock)
: m_clock(std::move(clock))
{
}

std::string CLineStamper::Feed(const char *buf, std::size_t n)
{
	std::string out;
	out.reserve(n + 32);
	std::size_t i = 0;
	while (i < n) {
		if (m_atLineStart) {
			// \r too, so a Windows "\r\n" blank line stays unstamped.
			if (buf[i] == '\n' || buf[i] == '\r') {
				out += buf[i++];
				continue;
			}
			out += m_clock();
			m_atLineStart = false;
		}
		const void *nl = std::memchr(buf + i, '\n', n - i);
		const std::size_t end =
			nl != nullptr ? static_cast<std::size_t>(static_cast<const char *>(nl) - buf) + 1 : n;
		out.append(buf + i, end - i);
		i = end;
		m_atLineStart = nl != nullptr;
	}
	return out;
}

std::string CLineAssembler::Take(const char *buf, std::size_t n)
{
	m_pending.append(buf, n);
	std::size_t end = m_pending.rfind('\n');
	if (end == std::string::npos) {
		if (m_pending.size() < kMaxPending) {
			return std::string();
		}
		end = m_pending.size() - 1;
	}
	std::string whole = m_pending.substr(0, end + 1);
	m_pending.erase(0, end + 1);
	return whole;
}

std::string CLineAssembler::Flush()
{
	std::string rest;
	rest.swap(m_pending);
	// Terminated, or the other stream's next line would land on the end of it.
	if (!rest.empty() && rest.back() != '\n') {
		rest += '\n';
	}
	return rest;
}

// CRotatingLog  (portable C stdio)

CRotatingLog::~CRotatingLog()
{
	Close();
	if (m_crashFd >= 0) {
		OsClose(m_crashFd);
		m_crashFd = -1;
	}
}

int CRotatingLog::CrashFd() const
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(m_mx);
	return m_fp != nullptr ? OsFileno(m_fp) : -1;
#else
	return m_crashFd;
#endif
}

void CRotatingLog::PointCrashFd()
{
	// m_mx held by caller. Reserve a number on the first open and only ever re-point it at the
	// current file. Never closing it is the point: a number that gets freed can be handed to a
	// client socket on another thread before the crash handler uses it.
	//
	// Not on Windows. There, a file with an open handle cannot be renamed, so holding this would
	// make Rotate()'s rename() fail and the reopen truncate the log instead of rotating it. The
	// descriptor buys nothing there either: wx handles fatal errors through SEH and calls
	// ExitProcess() rather than raising SIGABRT, so the handler this feeds never runs.
#ifndef _WIN32
	// A failed reopen leaves the reserved descriptor on the file just rotated to "<path>.1".
	// That is deliberate: it is still a real file on disk, so a crash report goes somewhere
	// rather than nowhere, which matters most for amuleapi where this is the only sink.
	if (m_fp == nullptr) {
		return;
	}
	const int fd = OsFileno(m_fp);
	if (m_crashFd < 0) {
		m_crashFd = OsDup(fd);
	} else {
		OsDup2(fd, m_crashFd);
	}
	// Never closed, so without FD_CLOEXEC it would follow every exec'd child and keep the log
	// file's inode alive there. dup() and dup2() both clear the flag.
	if (m_crashFd >= 0) {
		(void)fcntl(m_crashFd, F_SETFD, FD_CLOEXEC);
	}
#endif
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
	PointCrashFd();
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
	PointCrashFd();
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

// CLogTee

CLogTee::~CLogTee()
{
	Uninstall();
}

bool CLogTee::Install(const std::string &logPath, std::size_t maxBytes)
{
	if (m_installed) {
		return false;
	}
	if (!logPath.empty() && !m_log.Open(logPath, maxBytes)) {
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

	// On a TTY stdout was line-buffered; once fd 1 is a pipe libc switches it to full
	// buffering, which would delay console and file output until 4-8 KB accumulate. Force line
	// buffering back; stderr stays unbuffered.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);

	m_installed = true;
	m_outThread = std::thread(&CLogTee::Pump, this, m_pipeOutRead, m_savedOut);
	m_errThread = std::thread(&CLogTee::Pump, this, m_pipeErrRead, m_savedErr);
	return true;
}

void CLogTee::Pump(int readFd, int consoleFd)
{
	char buf[kReadChunk];
	CLineStamper stamper;
	CLineAssembler lines;
	for (;;) {
		const long n = OsRead(readFd, buf, sizeof(buf));
		if (n > 0) {
			const std::string out = stamper.Feed(buf, static_cast<std::size_t>(n));
			// Console first so the terminal keeps behaving as it did, then the
			// file copy (locked + rotated inside CRotatingLog). The file takes whole
			// lines only: the other stream's pump writes to it too.
			WriteAll(consoleFd, out.data(), out.size());
			const std::string whole = lines.Take(out.data(), out.size());
			m_log.Write(whole.data(), whole.size());
		} else if (n == 0) {
			break; // write end closed
		} else if (RetryErrno()) {
			continue;
		} else {
			break;
		}
	}
	const std::string rest = lines.Flush();
	m_log.Write(rest.data(), rest.size());
}

void CLogTee::RedirectStderrForCrash()
{
	// CrashFd() takes no lock, which matters: this runs from a fatal signal handler.
	int fd = m_log.CrashFd();
	if (fd < 0) {
		fd = m_savedErr;
	}
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
